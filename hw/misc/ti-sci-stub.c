/*
 * TI AM64x secure proxy + stub DMSC (TISCI firmware).
 * Just enough for Linux ti-msgmgr (am654-secure-proxy) and ti_sci: every
 * request on tx thread 13 is ACKed on rx thread 12 with a canned payload.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"

#define TYPE_TI_SCI_STUB "ti-sci-stub"
OBJECT_DECLARE_SIMPLE_TYPE(TISciStub, TI_SCI_STUB)

#define TX_THREAD 13
#define RX_THREAD 12
#define MSG_WORDS 15

#define MSG_VERSION          0x0002
#define MSG_GET_DEVICE_STATE 0x0201
#define MSG_GET_CLOCK_STATE  0x0101
#define MSG_GET_CLOCK_FREQ   0x010e
#define MSG_QUERY_CLOCK_FREQ 0x010d
#define MSG_GET_RES_RANGE    0x1500

struct TISciStub {
    SysBusDevice parent_obj;
    MemoryRegion data, rt, scfg;
    qemu_irq irq;
    uint32_t tx[MSG_WORDS];
    uint32_t rx[MSG_WORDS];
    bool rx_pending;
};

static void sci_update_irq(TISciStub *s)
{
    qemu_set_irq(s->irq, s->rx_pending);
}

/* shortcut: every clock reports 48 MHz (UART functional clock), refine per clk id if a driver needs it */
static void sci_handle(TISciStub *s)
{
    uint8_t *rq = (uint8_t *)s->tx, *rp = (uint8_t *)s->rx;
    uint16_t type = lduw_le_p(rq);

    memset(rp, 0, sizeof(s->rx));
    memcpy(rp, rq, 4);                 /* type, host, seq */
    stl_le_p(rp + 4, 1 << 1);          /* TI_SCI_FLAG_RESP_GENERIC_ACK */

    switch (type) {
    case MSG_VERSION:
        memcpy(rp + 8, "qemu ti-sci stub", 16);
        stw_le_p(rp + 40, 1);          /* firmware_revision */
        rp[42] = 3;                    /* abi_major */
        rp[43] = 1;
        break;
    case MSG_GET_DEVICE_STATE:
        rp[16] = 1;                    /* programmed_state: ON */
        rp[17] = 1;                    /* current_state: ON */
        break;
    case MSG_GET_CLOCK_STATE:
        rp[8] = rp[9] = 1;             /* READY */
        break;
    case MSG_GET_CLOCK_FREQ:
        stq_le_p(rp + 8, 48000000);
        break;
    case MSG_QUERY_CLOCK_FREQ:
        stq_le_p(rp + 8, ldq_le_p(rq + 20));  /* target */
        break;
    case MSG_GET_RES_RANGE:
        stw_le_p(rp + 8, 0);           /* range_start */
        stw_le_p(rp + 10, 64);         /* range_num */
        break;
    default:
        break;                         /* plain ACK */
    }
    s->rx_pending = true;
    sci_update_irq(s);
}

static uint64_t data_read(void *opaque, hwaddr addr, unsigned size)
{
    TISciStub *s = opaque;
    unsigned thread = addr >> 12, reg = ((addr & 0xfff) - 4) / 4;

    if (thread != RX_THREAD || (addr & 0xfff) < 4 || reg >= MSG_WORDS) {
        return 0;
    }
    if (reg == MSG_WORDS - 1) {        /* last word read pops the message */
        s->rx_pending = false;
        sci_update_irq(s);
    }
    return s->rx[reg];
}

static void data_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TISciStub *s = opaque;
    unsigned thread = addr >> 12, reg = ((addr & 0xfff) - 4) / 4;

    if (thread != TX_THREAD || (addr & 0xfff) < 4 || reg >= MSG_WORDS) {
        return;
    }
    s->tx[reg] = val;
    if (reg == MSG_WORDS - 1) {
        sci_handle(s);
    }
}

static uint64_t rt_read(void *opaque, hwaddr addr, unsigned size)
{
    TISciStub *s = opaque;

    if ((addr & 0xfff)) {
        return 0;
    }
    switch (addr >> 12) {
    case TX_THREAD:
        return 1;                      /* one send credit, always */
    case RX_THREAD:
        return s->rx_pending;
    }
    return 0;
}

static uint64_t scfg_read(void *opaque, hwaddr addr, unsigned size)
{
    /* ctrl of thread t at 0x1000 * (t + 1); bit 31 = direction (1 = rx) */
    return addr == 0x1000 * (RX_THREAD + 1) ? 1u << 31 : 0;
}

static void nop_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps data_ops = {
    .read = data_read, .write = data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static const MemoryRegionOps rt_ops = {
    .read = rt_read, .write = nop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static const MemoryRegionOps scfg_ops = {
    .read = scfg_read, .write = nop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void sci_init(Object *obj)
{
    TISciStub *s = TI_SCI_STUB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* order matches sysbus_mmio_map indices: target_data, rt, scfg */
    memory_region_init_io(&s->data, obj, &data_ops, s, "ti-sci.target_data", 0x80000);
    memory_region_init_io(&s->rt, obj, &rt_ops, s, "ti-sci.rt", 0x80000);
    memory_region_init_io(&s->scfg, obj, &scfg_ops, s, "ti-sci.scfg", 0x80000);
    sysbus_init_mmio(sbd, &s->data);
    sysbus_init_mmio(sbd, &s->rt);
    sysbus_init_mmio(sbd, &s->scfg);
    sysbus_init_irq(sbd, &s->irq);
}

static const TypeInfo sci_info = {
    .name = TYPE_TI_SCI_STUB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TISciStub),
    .instance_init = sci_init,
};

static void sci_register_types(void)
{
    type_register_static(&sci_info);
}
type_init(sci_register_types)

/*
 * ID-only OMAP I2C (IP v2) stub: lets i2c-omap probe and reset the
 * controller; bus transfers are not modelled (they time out).
 * shortcut: no I2C bus, add one when a peer on i2c is needed.
 */
#define TYPE_TI_I2C_ID "ti-am64x-i2c-id"

static uint64_t i2c_id_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case 0x04:                         /* REVNB_LO: scheme 1, v2 layout */
        return 0x4000 | (1 << 8);
    case 0x90:                         /* SYSS: reset done */
        return 1;
    }
    return 0;
}

static const MemoryRegionOps i2c_id_ops = {
    .read = i2c_id_read, .write = nop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2, .valid.max_access_size = 4,
};

static void i2c_id_init(Object *obj)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);

    memory_region_init_io(mr, obj, &i2c_id_ops, NULL, "ti-am64x.i2c-id", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), mr);
}

static const TypeInfo i2c_id_info = {
    .name = TYPE_TI_I2C_ID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SysBusDevice),
    .instance_init = i2c_id_init,
};

static void i2c_id_register_types(void)
{
    type_register_static(&i2c_id_info);
}
type_init(i2c_id_register_types)
