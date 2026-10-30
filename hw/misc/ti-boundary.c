/*
 * Boundary tap for one shared TI block (ti-am64x mailbox@29020000).
 *
 * The A53 side is QEMU. The block is owned by the R5F side (pyrite). In
 * record mode every A-side access and every interrupt-line change is written
 * to a trace file, keyed by the boundary access count. Reads return 0 because
 * no R-side model is attached. In replay mode the next trace record must match
 * each A-side access (direction, offset, size, and value for writes) and reads
 * return the recorded value. Interrupt lines fire at their recorded count.
 *
 * Trace format (text, one record per line), see docs/ti-boundary-trace.md:
 *   ti-boundary-trace 1 base=0x29020000 size=0x200
 *   a <n> <r|w> <offset-hex> <size> <value-hex>
 *   i <n> <line> <level>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"

#define TYPE_TI_BOUNDARY "ti-boundary"
OBJECT_DECLARE_SIMPLE_TYPE(TiBoundaryState, TI_BOUNDARY)

#define TI_BOUNDARY_SIZE 0x200
#define TI_BOUNDARY_IRQS 2
#define TI_BOUNDARY_MAGIC "ti-boundary-trace 1"

typedef struct {
    uint64_t n;
    bool write;
    uint64_t off;
    unsigned size;
    uint64_t val;
} TiBoundaryAccess;

typedef struct {
    uint64_t n;
    unsigned line;
    bool level;
} TiBoundaryIrq;

struct TiBoundaryState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq out[TI_BOUNDARY_IRQS];
    uint64_t base;
    char *record;
    char *replay;

    FILE *rec;
    uint64_t count;

    TiBoundaryAccess *acc;
    size_t nacc, next_acc;
    TiBoundaryIrq *irq;
    size_t nirq, next_irq;
};

static void tb_record_access(TiBoundaryState *s, bool write, uint64_t off,
                             unsigned size, uint64_t val)
{
    if (s->rec) {
        fprintf(s->rec, "a %" PRIu64 " %c %" PRIx64 " %u %" PRIx64 "\n",
                s->count + 1, write ? 'w' : 'r', off, size, val);
    }
}

static void tb_fire_irqs(TiBoundaryState *s)
{
    while (s->next_irq < s->nirq && s->irq[s->next_irq].n == s->count) {
        TiBoundaryIrq *i = &s->irq[s->next_irq++];
        qemu_set_irq(s->out[i->line], i->level);
    }
}

/* Replay: the access must be the next record. Divergence is fatal. */
static uint64_t tb_replay_access(TiBoundaryState *s, bool write, uint64_t off,
                                 unsigned size, uint64_t val)
{
    TiBoundaryAccess *a;

    if (s->next_acc >= s->nacc) {
        error_report("ti-boundary: replay trace ended at access %" PRIu64
                     " (offset 0x%" PRIx64 ")", s->count + 1, off);
        exit(1);
    }
    a = &s->acc[s->next_acc];
    if (a->write != write || a->off != off || a->size != size ||
        (write && a->val != val)) {
        error_report("ti-boundary: access %" PRIu64 " diverges from trace: "
                     "got %c 0x%" PRIx64 " size %u val 0x%" PRIx64
                     ", trace has %c 0x%" PRIx64 " size %u val 0x%" PRIx64,
                     s->count + 1, write ? 'w' : 'r', off, size, val,
                     a->write ? 'w' : 'r', a->off, a->size, a->val);
        exit(1);
    }
    s->next_acc++;
    return write ? 0 : a->val;
}

static void tb_access_done(TiBoundaryState *s)
{
    s->count++;
    tb_fire_irqs(s);
}

static uint64_t tb_read(void *opaque, hwaddr off, unsigned size)
{
    TiBoundaryState *s = opaque;
    uint64_t val = 0;

    if (s->replay) {
        val = tb_replay_access(s, false, off, size, 0);
    }
    tb_record_access(s, false, off, size, val);
    tb_access_done(s);
    return val;
}

static void tb_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    TiBoundaryState *s = opaque;

    if (s->replay) {
        tb_replay_access(s, true, off, size, val);
    }
    tb_record_access(s, true, off, size, val);
    tb_access_done(s);
}

static const MemoryRegionOps tb_ops = {
    .read = tb_read,
    .write = tb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* R-side interrupt line: logged in record mode, forwarded to the GIC. */
static void tb_irq_in(void *opaque, int line, int level)
{
    TiBoundaryState *s = opaque;

    if (s->rec) {
        fprintf(s->rec, "i %" PRIu64 " %d %d\n", s->count, line, level);
    }
    qemu_set_irq(s->out[line], level);
}

static bool tb_load(TiBoundaryState *s, Error **errp)
{
    g_autofree char *text = NULL;
    g_autofree char *header = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) gerr = NULL;
    uint64_t n, last_n = 0;

    if (!g_file_get_contents(s->replay, &text, NULL, &gerr)) {
        error_setg(errp, "%s: %s", s->replay, gerr->message);
        return false;
    }
    header = g_strdup_printf(TI_BOUNDARY_MAGIC " base=0x%" PRIx64
                             " size=0x%x", s->base, TI_BOUNDARY_SIZE);
    lines = g_strsplit(text, "\n", -1);
    if (!lines[0] || strcmp(lines[0], header) != 0) {
        error_setg(errp, "%s: bad header, want \"%s\"", s->replay, header);
        return false;
    }
    for (unsigned line_no = 2; lines[line_no - 1]; line_no++) {
        const char *l = lines[line_no - 1];
        char kind, dir[2];
        uint64_t off, size, val, line, level;

        if (!*l) {
            continue;
        }
        if (sscanf(l, "a %" SCNu64 " %1s %" SCNx64 " %" SCNu64 " %" SCNx64,
                   &n, dir, &off, &size, &val) == 5 &&
            (dir[0] == 'r' || dir[0] == 'w') && size >= 1 && size <= 4 &&
            (size & (size - 1)) == 0 && n == s->nacc + 1) {
            s->acc = g_renew(TiBoundaryAccess, s->acc, s->nacc + 1);
            s->acc[s->nacc++] = (TiBoundaryAccess){
                .n = n, .write = dir[0] == 'w', .off = off, .size = size,
                .val = val,
            };
        } else if (sscanf(l, "%c %" SCNu64 " %" SCNu64 " %" SCNu64,
                          &kind, &n, &line, &level) == 4 && kind == 'i' &&
                   line < TI_BOUNDARY_IRQS && level <= 1 && n >= last_n) {
            last_n = n;
            s->irq = g_renew(TiBoundaryIrq, s->irq, s->nirq + 1);
            s->irq[s->nirq++] = (TiBoundaryIrq){
                .n = n, .line = line, .level = level,
            };
        } else {
            error_setg(errp, "%s: bad record on line %u: %s", s->replay,
                       line_no, l);
            return false;
        }
    }
    return true;
}

static void tb_realize(DeviceState *dev, Error **errp)
{
    TiBoundaryState *s = TI_BOUNDARY(dev);

    if (s->record && s->replay) {
        error_setg(errp, "ti-boundary: record and replay are exclusive");
        return;
    }
    if (s->record) {
        s->rec = fopen(s->record, "w");
        if (!s->rec) {
            error_setg_errno(errp, errno, "ti-boundary: %s", s->record);
            return;
        }
        setvbuf(s->rec, NULL, _IOLBF, 0);
        fprintf(s->rec, TI_BOUNDARY_MAGIC " base=0x%" PRIx64 " size=0x%x\n",
                s->base, TI_BOUNDARY_SIZE);
    }
    if (s->replay && !tb_load(s, errp)) {
        return;
    }
    qdev_init_gpio_in(dev, tb_irq_in, TI_BOUNDARY_IRQS);
}

static void tb_init(Object *obj)
{
    TiBoundaryState *s = TI_BOUNDARY(obj);

    memory_region_init_io(&s->mmio, obj, &tb_ops, s, TYPE_TI_BOUNDARY,
                          TI_BOUNDARY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->out[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->out[1]);
}

/* Reset rewinds the replay and fires interrupt records with n == 0. */
static void tb_reset(DeviceState *dev)
{
    TiBoundaryState *s = TI_BOUNDARY(dev);

    s->count = s->next_acc = s->next_irq = 0;
    tb_fire_irqs(s);
}

static const Property tb_props[] = {
    DEFINE_PROP_UINT64("base", TiBoundaryState, base, 0),
    DEFINE_PROP_STRING("record", TiBoundaryState, record),
    DEFINE_PROP_STRING("replay", TiBoundaryState, replay),
};

static void tb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = tb_realize;
    device_class_set_legacy_reset(dc, tb_reset);
    device_class_set_props(dc, tb_props);
    dc->user_creatable = false;
}

static const TypeInfo tb_info = {
    .name = TYPE_TI_BOUNDARY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TiBoundaryState),
    .instance_init = tb_init,
    .class_init = tb_class_init,
};

static void tb_register_types(void)
{
    type_register_static(&tb_info);
}

type_init(tb_register_types)
