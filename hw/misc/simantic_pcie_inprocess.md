# In-process Verilated co-sim — design sketch

This is a **design note, not built code.** It shows how the socket-based
`simantic_pcie_proxy.c` would collapse into an in-process device that links the
Verilated model directly and calls `eval()` — the QEMU analogue of Renode's
`LibraryConnection`. It is the lowest-latency, structurally-deadlock-free
variant (Option 3 in the trade-off discussion).

## Why it is deadlock-free by construction

The socket design's only deadlock risk was *holding the BQL across a blocking
socket wait*. In-process there is **no blocking wait at all**: a BAR access is a
direct function call into the Verilated model that ticks a bounded number of
cycles and returns. `eval()` never blocks, so "BQL held across a blocking op"
cannot occur. DMA still goes to guest RAM off-BQL under RCU (same as the socket
version), so the sender path never contends for the BQL either.

## Latency

| Path | Socket proxy | In-process |
|---|---|---|
| BAR read/write | ~10–20 µs (TCP RTT) | ~0.05–0.2 µs (direct call) |
| Free-running tick | one RTT per quantum | inline, no IPC |

~100× faster per access. Cost: no process isolation — a Verilator assertion or
segfault takes QEMU down with it. Acceptable for CI / dev; not for running
untrusted models.

## The model-side ABI

The Verilated model is compiled to a shared library exporting a tiny C ABI
instead of running `RenodeAgent::simulate()` over sockets. One clean way: a
thin `extern "C"` wrapper around the `PcieEndpoint` bus class so the same bus
logic is reused unchanged.

```cpp
// sim_inprocess.cpp — compiled into libVmyblock_inprocess.so
#include "Vmyblock.h"
#include "src/buses/pcie-endpoint.h"

static Vmyblock*    top;
static PcieEndpoint bus;

extern "C" {

// Called once at device realize().
void sim_init(void) {
    top = new Vmyblock();
    bus.clk = &top->clk;  bus.rst = &top->rst;
    bus.bar_addr = &top->bar_addr; /* ... wire all signals ... */
    bus.irq = &top->irq;
    bus.bind_eval([]{ top->eval(); });   // PcieEndpoint::evaluateModel
    bus.reset();
}

// BAR access — synchronous, returns immediately.
uint32_t sim_bar_read (uint64_t off)            { return bus.read(4, off); }
void     sim_bar_write(uint64_t off, uint32_t v){ bus.write(4, off, v);   }

// Free-running advance — N cycles. DMA + IRQ callbacks fire during the ticks.
void sim_tick(uint64_t n) { bus.tick(true, n); }

// Host wires these so the bus class can reach guest memory / interrupts
// directly instead of going over the sender socket.
void sim_set_dma_write_cb(void (*cb)(uint64_t addr, uint32_t data));
void sim_set_dma_read_cb (uint32_t (*cb)(uint64_t addr));
void sim_set_irq_cb      (void (*cb)(unsigned vector, int level));

}
```

`PcieEndpoint`'s `handleDma()` / interrupt path would, in this build, call the
registered callbacks instead of `pushDoubleWordToAgent` / `handleInterrupts`.
That is the only behavioural change to the bus class — guarded by a build flag
so the socket build is untouched.

## The device side (replaces the socket proxy)

```c
// simantic_pcie_inprocess.c
#include "qemu/osdep.h"
#include "qemu/rcu.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include <dlfcn.h>

typedef struct {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    void *lib;                              // dlopen handle
    void     (*sim_init)(void);
    uint32_t (*sim_bar_read)(uint64_t);
    void     (*sim_bar_write)(uint64_t, uint32_t);
    void     (*sim_tick)(uint64_t);
    QEMUTimer *tick_timer;
    uint64_t   tick_period_ns;
    uint64_t   tick_batch;
    char      *sim_lib_path;
    uint32_t   bar0_size;
} SimanticInprocState;

// --- DMA callbacks the model calls directly (off-BQL, RCU) ---
static SimanticInprocState *g_dev;   // single instance for the C callback ABI

static void dma_write_cb(uint64_t addr, uint32_t data) {
    WITH_RCU_READ_LOCK_GUARD() {
        pci_dma_write(&g_dev->parent_obj, addr, &data, sizeof(data));
    }
}
static uint32_t dma_read_cb(uint64_t addr) {
    uint32_t data = 0;
    WITH_RCU_READ_LOCK_GUARD() {
        pci_dma_read(&g_dev->parent_obj, addr, &data, sizeof(data));
    }
    return data;
}
static void irq_cb(unsigned vector, int level) {
    if (level && msix_enabled(&g_dev->parent_obj))
        msix_notify(&g_dev->parent_obj, vector);
}

// --- BAR MMIO: direct calls, no socket, returns immediately ---
static uint64_t bar0_read(void *opaque, hwaddr off, unsigned size) {
    SimanticInprocState *s = opaque;
    return s->sim_bar_read(off);          // ~100 ns
}
static void bar0_write(void *opaque, hwaddr off, uint64_t val, unsigned size) {
    SimanticInprocState *s = opaque;
    s->sim_bar_write(off, (uint32_t)val);
}
static const MemoryRegionOps bar0_ops = {
    .read = bar0_read, .write = bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

// --- Free-running clock: a virtual-time timer ticking the model inline ---
// No BQL dance needed: sim_tick() does not block, and DMA inside it goes
// off-BQL via the RCU callbacks above. The timer callback runs under the BQL,
// which is fine because it never waits on anything.
static void tick_cb(void *opaque) {
    SimanticInprocState *s = opaque;
    s->sim_tick(s->tick_batch);
    timer_mod(s->tick_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->tick_period_ns);
}

static void realize(PCIDevice *dev, Error **errp) {
    SimanticInprocState *s = SIMANTIC_INPROC(dev);
    g_dev = s;

    s->lib = dlopen(s->sim_lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!s->lib) { error_setg(errp, "dlopen: %s", dlerror()); return; }
    s->sim_init      = dlsym(s->lib, "sim_init");
    s->sim_bar_read  = dlsym(s->lib, "sim_bar_read");
    s->sim_bar_write = dlsym(s->lib, "sim_bar_write");
    s->sim_tick      = dlsym(s->lib, "sim_tick");
    // wire the callbacks into the model
    ((void(*)(void*))dlsym(s->lib, "sim_set_dma_write_cb"))(dma_write_cb);
    ((void(*)(void*))dlsym(s->lib, "sim_set_dma_read_cb")) (dma_read_cb);
    ((void(*)(void*))dlsym(s->lib, "sim_set_irq_cb"))      (irq_cb);
    s->sim_init();

    memory_region_init_io(&s->bar0, OBJECT(s), &bar0_ops, s,
                          "simantic-bar0", s->bar0_size);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    msix_init_exclusive_bar(dev, 1, 1, errp);
    msix_vector_use(dev, 0);

    s->tick_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick_cb, s);
    timer_mod(s->tick_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->tick_period_ns);
}
```

## What carries over vs. the socket version

| Component | Socket proxy | In-process |
|---|---|---|
| `PcieEndpoint` bus class | as-is | as-is, DMA/IRQ via callbacks (build flag) |
| BAR transport | TCP round-trip | direct `dlsym` call |
| DMA to RAM | sender thread, off-BQL RCU | callback, off-BQL RCU (identical) |
| Free-running clock | proxy `TickClock` over socket | `sim_tick()` inline in timer |
| MSI-X | `interrupt` msg → `msix_notify` | `irq_cb` → `msix_notify` |
| Isolation | model in its own process | none (shared address space) |
| BAR latency | ~10–20 µs | ~0.1 µs |

## Determinism note

Both variants keep the virtual-time tick model, so both stay reproducible.
In-process is *more* deterministic in practice: no TCP scheduling jitter between
the tick quantum and the model advancing — the tick is a synchronous call inside
the QEMU virtual-time callback.

## Recommended path

Ship the socket proxy first (isolation, easier to debug the wire protocol).
Add this in-process variant behind the same `-device` family as a
`simantic-pcie-inprocess` type once the protocol is validated and someone needs
the ~100× latency win for a DMA-heavy model in CI. The bus class and the
off-BQL DMA pattern are shared, so the marginal cost is the thin C ABI wrapper
plus this device file.
```
