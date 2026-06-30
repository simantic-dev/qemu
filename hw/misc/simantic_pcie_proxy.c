/*
 * simantic-pcie-proxy — QEMU PCIe device that bridges BAR accesses and DMA
 * to a Verilated RTL model via the Renode co-simulation protocol.
 *
 * Architecture:
 *   Linux guest driver
 *       ↕  QEMU PCIe root complex (existing, unmodified)
 *   simantic_pcie_proxy  ← this file
 *       ↕  TCP sockets (Protocol structs, same wire format as Renode plugin)
 *   Verilated RTL (user's endpoint logic + pcie-endpoint bus class)
 *
 * TLP mapping:
 *   MWr  (BAR write from host)  → writeRequestDoubleWord(offset, data) → ok
 *   MRd  (BAR read from host)   → readRequestDoubleWord(offset) → readRequest(data)
 *   DMA write (device → host)   → RTL sends pushDoubleWord(hostAddr, data) → pci_dma_write → pushConfirmation
 *   DMA read  (device ← host)   → RTL sends getDoubleWord(hostAddr) → pci_dma_read → writeRequest(data)
 *   MSI-X                       → RTL sends interrupt(addr=0, value=1) → msix_notify(dev, 0)
 *
 * The co-sim protocol uses two TCP connections:
 *   main_fd   — QEMU-initiated (BAR read/write requests)
 *   sender_fd — RTL-initiated  (DMA, interrupt); received in a QemuThread
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/rcu.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ── Protocol (must match Renode's ProtocolMessage and renode.h) ── */

#pragma pack(push, 1)
typedef struct {
    int32_t  action_id;
    uint64_t addr;
    uint64_t value;
    int32_t  peripheral_index;
} Protocol;
#pragma pack(pop)

/* Action IDs — must stay in sync with renode_action_enumerators.svh */
enum {
    ACTION_INVALID           = 0,
    ACTION_TICK_CLOCK        = 1,
    ACTION_WRITE_REQUEST     = 2,   /* legacy 32-bit write response */
    ACTION_READ_REQUEST      = 3,   /* legacy 32-bit read response  */
    ACTION_RESET_PERIPHERAL  = 4,
    ACTION_LOG_MESSAGE       = 5,
    ACTION_INTERRUPT         = 6,
    ACTION_DISCONNECT        = 7,
    ACTION_ERROR             = 8,
    ACTION_OK                = 9,
    ACTION_HANDSHAKE         = 10,
    ACTION_PUSH_DWORD        = 11,  /* RTL→host DMA write */
    ACTION_GET_DWORD         = 12,  /* RTL→host DMA read  */
    ACTION_PUSH_WORD         = 13,
    ACTION_GET_WORD          = 14,
    ACTION_PUSH_BYTE         = 15,
    ACTION_GET_BYTE          = 16,
    ACTION_READ_REQ_BYTE     = 21,
    ACTION_READ_REQ_WORD     = 22,
    ACTION_READ_REQ_DWORD    = 23,
    ACTION_READ_REQ_QWORD    = 24,
    ACTION_WRITE_REQ_BYTE    = 25,
    ACTION_WRITE_REQ_WORD    = 26,
    ACTION_WRITE_REQ_DWORD   = 27,
    ACTION_WRITE_REQ_QWORD   = 28,
    ACTION_PUSH_QWORD        = 29,
    ACTION_GET_QWORD         = 30,
    ACTION_PUSH_CONFIRM      = 31,
};

/* ── Device state ─────────────────────────────────────────────── */

#define TYPE_SIMANTIC_PCIE_PROXY "simantic-pcie-proxy"
OBJECT_DECLARE_SIMPLE_TYPE(SimanticPcieProxyState, SIMANTIC_PCIE_PROXY)

#define SIMANTIC_MSIX_VECTORS    1
#define SIMANTIC_MSIX_BAR        1   /* BAR 1 reserved for MSI-X table */

struct SimanticPcieProxyState {
    PCIDevice parent_obj;

    MemoryRegion bar0;          /* user register space */

    /* socket fds */
    int main_fd;                /* QEMU→RTL: BAR requests */
    int sender_fd;              /* RTL→QEMU: DMA + interrupts (async) */

    QemuThread sender_thread;
    bool running;
    QemuMutex main_tx_lock;     /* serialises main_fd writes (vCPU/timer vs sender thread) */

    /* device properties */
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0_size;
    uint16_t main_port;         /* port for main (QEMU→RTL) socket */
    uint16_t sender_port;       /* port for sender (RTL→QEMU) socket */
    char    *host;
};

/* ── Socket helpers ───────────────────────────────────────────── */

static int tcp_connect(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        error_report("simantic-pcie-proxy: socket() failed: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    inet_pton(AF_INET, host, &sa.sin_addr);

    if(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        error_report("simantic-pcie-proxy: connect(%s:%u) failed: %s",
                     host, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool proto_send(int fd, const Protocol *msg)
{
    ssize_t n = send(fd, msg, sizeof(*msg), MSG_NOSIGNAL);
    return n == (ssize_t)sizeof(*msg);
}

static bool proto_recv(int fd, Protocol *msg)
{
    ssize_t total = 0;
    uint8_t *buf = (uint8_t *)msg;
    while(total < (ssize_t)sizeof(*msg)) {
        ssize_t n = recv(fd, buf + total, sizeof(*msg) - total, 0);
        if(n <= 0) return false;
        total += n;
    }
    return true;
}

/* ── BAR 0 MMIO callbacks (MWr / MRd TLP equivalents) ─────────── */

static uint64_t simantic_bar0_read(void *opaque, hwaddr offset, unsigned size)
{
    SimanticPcieProxyState *s = opaque;

    Protocol req = {
        .action_id        = ACTION_READ_REQ_DWORD,
        .addr             = offset,
        .value            = 0,
        .peripheral_index = 0,
    };

    /* The sender thread may interleave a DMA-response write on main_fd while
     * the RTL services this read (RTL-initiated DMA inside a BAR access). Guard
     * the request send; the recv side is single-reader (BQL-serialised vCPU). */
    qemu_mutex_lock(&s->main_tx_lock);
    bool sent = proto_send(s->main_fd, &req);
    qemu_mutex_unlock(&s->main_tx_lock);
    if(!sent) {
        error_report("simantic-pcie-proxy: send readRequest failed");
        return ~0ULL;
    }

    Protocol resp;
    if(!proto_recv(s->main_fd, &resp)) {
        error_report("simantic-pcie-proxy: recv readResponse failed");
        return ~0ULL;
    }

    if(resp.action_id == ACTION_ERROR) {
        error_report("simantic-pcie-proxy: RTL returned error on read @ 0x%lx", offset);
        return ~0ULL;
    }

    return resp.value;
}

static void simantic_bar0_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    SimanticPcieProxyState *s = opaque;

    Protocol req = {
        .action_id        = ACTION_WRITE_REQ_DWORD,
        .addr             = offset,
        .value            = value,
        .peripheral_index = 0,
    };

    qemu_mutex_lock(&s->main_tx_lock);
    bool sent = proto_send(s->main_fd, &req);
    qemu_mutex_unlock(&s->main_tx_lock);
    if(!sent) {
        error_report("simantic-pcie-proxy: send writeRequest failed");
        return;
    }

    Protocol resp;
    if(!proto_recv(s->main_fd, &resp)) {
        error_report("simantic-pcie-proxy: recv writeAck failed");
        return;
    }

    if(resp.action_id == ACTION_ERROR) {
        error_report("simantic-pcie-proxy: RTL returned error on write @ 0x%lx", offset);
    }
}

static const MemoryRegionOps simantic_bar0_ops = {
    .read       = simantic_bar0_read,
    .write      = simantic_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ── Sender thread — RTL-initiated DMA and MSI-X ──────────────── */

static void simantic_handle_dma_write(SimanticPcieProxyState *s,
                                      uint64_t host_addr, uint32_t data)
{
    /* Run DMA-to-RAM off the BQL, under RCU — the same pattern QEMU's own
     * virtio/nvme devices use for iothread DMA. This is what makes the design
     * deadlock-safe by construction: the sender thread never contends for the
     * BQL, so a vCPU blocked in a BAR access (holding the BQL) can never block
     * this DMA. (Targets resolving to device MMIO rather than RAM would need
     * the BQL; for AI-accel / NVMe / SmartNIC workloads DMA targets host RAM.) */
    WITH_RCU_READ_LOCK_GUARD() {
        pci_dma_write(&s->parent_obj, host_addr, &data, sizeof(data));
    }

    /* The Verilated side reads pushConfirmation via receive(), which always
     * reads the MAIN socket (see socket_channel.cpp). So the confirmation must
     * be sent on main_fd, not sender_fd. main_tx_lock serialises main_fd writes
     * against the vCPU/timer threads. */
    Protocol conf = {
        .action_id        = ACTION_PUSH_CONFIRM,
        .addr             = 0,
        .value            = 0,
        .peripheral_index = 0,
    };
    qemu_mutex_lock(&s->main_tx_lock);
    proto_send(s->main_fd, &conf);
    qemu_mutex_unlock(&s->main_tx_lock);
}

static void simantic_handle_dma_read(SimanticPcieProxyState *s,
                                     uint64_t host_addr)
{
    uint32_t data = 0;
    /* Off-BQL under RCU — see simantic_handle_dma_write for rationale. */
    WITH_RCU_READ_LOCK_GUARD() {
        pci_dma_read(&s->parent_obj, host_addr, &data, sizeof(data));
    }

    /* requestFromAgent() on the Verilated side reads the data via receive()
     * (MAIN socket) and expects ACTION_WRITE_REQUEST. Send on main_fd. */
    Protocol resp = {
        .action_id        = ACTION_WRITE_REQUEST,
        .addr             = host_addr,
        .value            = data,
        .peripheral_index = 0,
    };
    qemu_mutex_lock(&s->main_tx_lock);
    proto_send(s->main_fd, &resp);
    qemu_mutex_unlock(&s->main_tx_lock);
}

static void *simantic_sender_thread(void *opaque)
{
    SimanticPcieProxyState *s = opaque;

    while(s->running) {
        Protocol msg;
        if(!proto_recv(s->sender_fd, &msg)) {
            if(s->running) {
                error_report("simantic-pcie-proxy: sender socket closed");
            }
            break;
        }

        switch(msg.action_id) {
        case ACTION_PUSH_DWORD:
            simantic_handle_dma_write(s, msg.addr, (uint32_t)msg.value);
            break;

        case ACTION_GET_DWORD:
            simantic_handle_dma_read(s, msg.addr);
            break;

        case ACTION_INTERRUPT:
            /* msg.addr = vector index, msg.value = 0 or 1 */
            if(msg.value && msix_enabled(&s->parent_obj)) {
                msix_notify(&s->parent_obj, (unsigned)msg.addr);
            }
            break;

        case ACTION_LOG_MESSAGE:
            /* RTL log — addr encodes string length, value encodes pointer.
             * In socket mode the string is sent as a subsequent message; for
             * simplicity we swallow it here. */
            break;

        case ACTION_DISCONNECT:
            s->running = false;
            break;

        default:
            break;
        }
    }
    return NULL;
}

/* ── Handshake ────────────────────────────────────────────────── */

static bool simantic_handshake(int fd)
{
    Protocol hs = { .action_id = ACTION_HANDSHAKE, .addr = 0, .value = 0, .peripheral_index = 0 };
    if(!proto_send(fd, &hs)) return false;
    Protocol resp;
    if(!proto_recv(fd, &resp)) return false;
    return resp.action_id == ACTION_OK;
}

/* ── Realize / Unrealize ──────────────────────────────────────── */

static void simantic_pcie_proxy_realize(PCIDevice *pci_dev, Error **errp)
{
    SimanticPcieProxyState *s = SIMANTIC_PCIE_PROXY(pci_dev);

    /* Connect sockets to Verilated model */
    s->main_fd = tcp_connect(s->host, s->main_port);
    if(s->main_fd < 0) {
        error_setg(errp, "simantic-pcie-proxy: cannot connect main socket to %s:%u",
                   s->host, s->main_port);
        return;
    }

    s->sender_fd = tcp_connect(s->host, s->sender_port);
    if(s->sender_fd < 0) {
        close(s->main_fd);
        error_setg(errp, "simantic-pcie-proxy: cannot connect sender socket to %s:%u",
                   s->host, s->sender_port);
        return;
    }

    if(!simantic_handshake(s->main_fd)) {
        error_setg(errp, "simantic-pcie-proxy: handshake failed on main socket");
        goto err_sockets;
    }

    /* BAR 0 — user register space */
    memory_region_init_io(&s->bar0, OBJECT(s), &simantic_bar0_ops, s,
                          "simantic-bar0", s->bar0_size);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /* MSI-X — single vector, hosted in BAR 1 */
    if(msix_init_exclusive_bar(pci_dev, SIMANTIC_MSIX_VECTORS,
                               SIMANTIC_MSIX_BAR, errp)) {
        goto err_sockets;
    }
    msix_vector_use(pci_dev, 0);

    /* Sender thread for RTL-initiated DMA and interrupts.
     * DMA runs off-BQL under RCU (see simantic_handle_dma_write), so no
     * dma serialisation lock is needed — the sender thread is the only caller
     * and RAM access is RCU-protected. */
    qemu_mutex_init(&s->main_tx_lock);
    s->running = true;
    qemu_thread_create(&s->sender_thread, "simantic-pcie-sender",
                       simantic_sender_thread, s, QEMU_THREAD_JOINABLE);
    return;

err_sockets:
    close(s->sender_fd);
    close(s->main_fd);
}

static void simantic_pcie_proxy_exit(PCIDevice *pci_dev)
{
    SimanticPcieProxyState *s = SIMANTIC_PCIE_PROXY(pci_dev);

    s->running = false;

    /* Send disconnect so the RTL model exits cleanly */
    Protocol disc = { .action_id = ACTION_DISCONNECT };
    proto_send(s->main_fd, &disc);

    shutdown(s->sender_fd, SHUT_RDWR);
    qemu_thread_join(&s->sender_thread);
    qemu_mutex_destroy(&s->main_tx_lock);

    close(s->main_fd);
    close(s->sender_fd);

    msix_uninit_exclusive_bar(pci_dev);
}

/* ── Device properties ────────────────────────────────────────── */

static Property simantic_pcie_proxy_props[] = {
    DEFINE_PROP_UINT16("vendor-id",   SimanticPcieProxyState, vendor_id,   0x1234),
    DEFINE_PROP_UINT16("device-id",   SimanticPcieProxyState, device_id,   0xBEEF),
    DEFINE_PROP_UINT32("bar0-size",   SimanticPcieProxyState, bar0_size,   4096),
    DEFINE_PROP_UINT16("main-port",   SimanticPcieProxyState, main_port,   9000),
    DEFINE_PROP_UINT16("sender-port", SimanticPcieProxyState, sender_port, 9001),
    DEFINE_PROP_STRING("host",        SimanticPcieProxyState, host),
    DEFINE_PROP_END_OF_LIST(),
};

/* ── Class init & type registration ───────────────────────────── */

static void simantic_pcie_proxy_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc    = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize    = simantic_pcie_proxy_realize;
    pc->exit       = simantic_pcie_proxy_exit;
    pc->class_id   = PCI_CLASS_OTHERS;

    device_class_set_props(dc, simantic_pcie_proxy_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "Simantic PCIe co-simulation proxy";
}

static void simantic_pcie_proxy_instance_init(Object *obj)
{
    SimanticPcieProxyState *s = SIMANTIC_PCIE_PROXY(obj);
    s->host = g_strdup("127.0.0.1");
}

static const TypeInfo simantic_pcie_proxy_info = {
    .name          = TYPE_SIMANTIC_PCIE_PROXY,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SimanticPcieProxyState),
    .instance_init = simantic_pcie_proxy_instance_init,
    .class_init    = simantic_pcie_proxy_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void simantic_pcie_proxy_register_types(void)
{
    type_register_static(&simantic_pcie_proxy_info);
}

type_init(simantic_pcie_proxy_register_types)
