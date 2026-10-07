#define pr_fmt(fmt) "plic: " fmt

#include <nuvix/irq.h>
#include <nuvix/cpu.h>
#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/libfdt.h>
#include <nuvix/math.h>
#include <nuvix/pgtable.h>
#include <nuvix/printk.h>
#include <nuvix/spinlock.h>
#include <nuvix/string.h>
#include <asm/csr.h>
#include <asm/io.h>

#define PLIC_MAX_SOURCES     1024
#define PLIC_MAX_CONTEXTS    15872
#define PLIC_PRIORITY_BASE   0
#define PLIC_PRIORITY_STRIDE 4
#define PLIC_ENABLE_BASE     0x2000
#define PLIC_ENABLE_STRIDE   0x80
#define PLIC_CONTEXT_BASE    0x200000
#define PLIC_CONTEXT_STRIDE  0x1000
#define PLIC_THRESHOLD       0
#define PLIC_CLAIM           4
#define PLIC_BITS_PER_WORD   32
#define PLIC_CONTEXT_NONE    ((unsigned int)-1)

static struct {
    int node;
    unsigned int nr_irqs;
    volatile uint32_t *enable;
    volatile void *claim;
    spinlock_t enable_lock;
} plic;

static uint64_t read_cells(const fdt32_t *cells, int count)
{
    uint64_t value = 0;

    for (int i = 0; i < count; ++i)
        value = (value << 32) | fdt32_to_cpu(cells[i]);
    return value;
}

static int plic_reg(const void *blob, int node, paddr_t *base, uint64_t *size)
{
    int parent = fdt_parent_offset(blob, node);
    int address_cells = fdt_address_cells(blob, parent);
    int size_cells = fdt_size_cells(blob, parent);
    const fdt32_t *reg;
    const void *ranges;
    int len;

    if ((address_cells != 1 && address_cells != 2) ||
        (size_cells != 1 && size_cells != 2))
        return -EOPNOTSUPP;
    /* QEMU virt's MMIO bus has no address translation. */
    if (parent != 0) {
        ranges = fdt_getprop(blob, parent, "ranges", &len);
        if (!ranges || len)
            return -EOPNOTSUPP;
    }
    reg = fdt_getprop(blob, node, "reg", &len);
    if (!reg || len != (address_cells + size_cells) * sizeof(*reg))
        return -EINVAL;
    *base = read_cells(reg, address_cells);
    *size = read_cells(reg + address_cells, size_cells);
    if ((*base & (PAGE_SIZE - 1)) || !*size)
        return -EINVAL;
    if (*size > UINT64_MAX - *base)
        return -EOVERFLOW;
    return 0;
}

static int plic_contexts(const void *blob, int node, unsigned int *contexts)
{
    const fdt32_t *extended;
    int len;

    for (unsigned int cpu = 0; cpu < nr_cpus; ++cpu)
        contexts[cpu] = PLIC_CONTEXT_NONE;
    extended = fdt_getprop(blob, node, "interrupts-extended", &len);
    if (!extended || len <= 0 || len % (2 * sizeof(*extended)))
        return -EINVAL;
    unsigned int count = len / (2 * sizeof(*extended));
    if (count > PLIC_MAX_CONTEXTS)
        return -EOPNOTSUPP;
    for (unsigned int context = 0; context < count; ++context) {
        const fdt32_t *cells, *reg;
        int parent, cpu_node, cpu_parent, address_cells, prop_len;
        unsigned long hart_id;

        parent = fdt_node_offset_by_phandle(
            blob, fdt32_to_cpu(extended[context * 2]));
        if (parent < 0)
            return -EINVAL;
        cells = fdt_getprop(blob, parent, "#interrupt-cells", &prop_len);
        if (!cells || prop_len != sizeof(*cells) || fdt32_to_cpu(*cells) != 1)
            return -EOPNOTSUPP;
        if (fdt32_to_cpu(extended[context * 2 + 1]) != SCAUSE_SEI)
            continue;
        if (fdt_node_check_compatible(blob, parent, "riscv,cpu-intc"))
            return -EOPNOTSUPP;
        cpu_node = fdt_parent_offset(blob, parent);
        cpu_parent = fdt_parent_offset(blob, cpu_node);
        address_cells = fdt_address_cells(blob, cpu_parent);
        if (address_cells != 1 && address_cells != 2)
            return -EOPNOTSUPP;
        reg = fdt_getprop(blob, cpu_node, "reg", &prop_len);
        if (!reg || prop_len != address_cells * sizeof(*reg))
            return -EINVAL;
        hart_id = read_cells(reg, address_cells);
        for (unsigned int cpu = 0; cpu < nr_cpus; ++cpu) {
            if (cpus[cpu].hart_id != hart_id)
                continue;
            if (contexts[cpu] != PLIC_CONTEXT_NONE)
                return -EEXIST;
            contexts[cpu] = context;
            break;
        }
    }
    for (unsigned int cpu = 0; cpu < nr_cpus; ++cpu) {
        if (contexts[cpu] == PLIC_CONTEXT_NONE)
            return -ENODEV;
    }
    return 0;
}

static int plic_map(paddr_t base, uint64_t size, uint64_t offset, size_t len)
{
    uint64_t start = ALIGN_DOWN(offset, (uint64_t)PAGE_SIZE);
    uint64_t end = ALIGN_UP(offset + len, (uint64_t)PAGE_SIZE);

    if (end > size)
        return -EINVAL;
    return pgtable_map_io(PLIC_VBASE + start, base + start, end - start);
}

int plic_init(const struct dtb *dtb)
{
    const void *blob = dtb->blob;
    const fdt32_t *ndev;
    const char *status;
    volatile uint32_t *priority;
    unsigned int contexts[CONFIG_NR_CPUS];
    unsigned int min_context, max_context, nr_irqs;
    paddr_t base;
    uint64_t size, enable_start, enable_end, control;
    int node, len, ret;

    node = fdt_node_offset_by_compatible(blob, -1, "sifive,plic-1.0.0");
    if (node < 0)
        return -ENODEV;
    status = fdt_getprop(blob, node, "status", &len);
    if (status && !((len == 5 && !memcmp(status, "okay", 5)) ||
                    (len == 3 && !memcmp(status, "ok", 3))))
        return -ENODEV;
    ndev = fdt_getprop(blob, node, "riscv,ndev", &len);
    if (!ndev || len != sizeof(*ndev))
        return -EINVAL;
    nr_irqs = fdt32_to_cpu(*ndev);
    if (!nr_irqs || nr_irqs >= PLIC_MAX_SOURCES)
        return -EOPNOTSUPP;
    ret = plic_reg(blob, node, &base, &size);
    if (ret)
        return ret;
    ret = plic_contexts(blob, node, contexts);
    if (ret)
        return ret;

    min_context = max_context = contexts[0];
    for (unsigned int cpu = 1; cpu < nr_cpus; ++cpu) {
        min_context = MIN(min_context, contexts[cpu]);
        max_context = MAX(max_context, contexts[cpu]);
    }
    enable_start = PLIC_ENABLE_BASE + min_context * PLIC_ENABLE_STRIDE;
    enable_end = PLIC_ENABLE_BASE + (max_context + 1) * PLIC_ENABLE_STRIDE;
    control = PLIC_CONTEXT_BASE + contexts[0] * PLIC_CONTEXT_STRIDE;
    ret = plic_map(base, size, PLIC_PRIORITY_BASE,
                   (nr_irqs + 1) * PLIC_PRIORITY_STRIDE);
    if (ret)
        return ret;
    ret = plic_map(base, size, enable_start, enable_end - enable_start);
    if (ret)
        return ret;
    ret = plic_map(base, size, control, PLIC_CLAIM + sizeof(uint32_t));
    if (ret)
        return ret;

    plic.node = node;
    plic.nr_irqs = nr_irqs;
    priority = (volatile uint32_t *)(PLIC_VBASE + PLIC_PRIORITY_BASE);
    plic.enable = (volatile uint32_t *)(PLIC_VBASE + PLIC_ENABLE_BASE +
                                        contexts[0] * PLIC_ENABLE_STRIDE);
    plic.claim = (volatile void *)(PLIC_VBASE + control + PLIC_CLAIM);
    spin_lock_init(&plic.enable_lock);
    for (unsigned int cpu = 0; cpu < nr_cpus; ++cpu) {
        volatile uint32_t *enable =
            (volatile uint32_t *)(PLIC_VBASE + PLIC_ENABLE_BASE +
                                  contexts[cpu] * PLIC_ENABLE_STRIDE);

        for (unsigned int word = 0;
             word < DIV_ROUND_UP(nr_irqs + 1, PLIC_BITS_PER_WORD); ++word)
            writel(0, enable + word);
    }
    for (unsigned int irq = 1; irq <= nr_irqs; ++irq)
        writel(1, priority + irq);
    writel(0, (volatile void *)(PLIC_VBASE + control + PLIC_THRESHOLD));
    pr_info("%u sources, CPU0 hart %p S-mode context %u\n", nr_irqs,
            (void *)cpus[0].hart_id, contexts[0]);
    return 0;
}

int plic_irq_parse(const struct dtb *dtb, int node, unsigned int *irq)
{
    const void *blob = dtb->blob;
    const fdt32_t *cells;
    int parent = node, controller, len;

    if (fdt_getprop(blob, node, "interrupts-extended", &len))
        return -EOPNOTSUPP;
    for (;;) {
        cells = fdt_getprop(blob, parent, "interrupt-parent", &len);
        if (cells)
            break;
        parent = fdt_parent_offset(blob, parent);
        if (parent < 0)
            return -EINVAL;
    }
    if (len != sizeof(*cells))
        return -EINVAL;
    controller = fdt_node_offset_by_phandle(blob, fdt32_to_cpu(*cells));
    if (controller < 0 || controller != plic.node)
        return -EOPNOTSUPP;
    cells = fdt_getprop(blob, controller, "#interrupt-cells", &len);
    if (!cells || len != sizeof(*cells) || fdt32_to_cpu(*cells) != 1)
        return -EOPNOTSUPP;
    cells = fdt_getprop(blob, node, "interrupts", &len);
    if (!cells || len != sizeof(*cells))
        return -EINVAL;
    *irq = fdt32_to_cpu(*cells);
    return 0;
}

unsigned int plic_nr_irqs(void)
{
    return plic.nr_irqs;
}

static int plic_toggle(unsigned int irq, bool enable)
{
    volatile uint32_t *reg;
    uint32_t mask, value;
    unsigned long flags;

    if (!irq || irq > plic.nr_irqs)
        return -EINVAL;
    reg = plic.enable + irq / PLIC_BITS_PER_WORD;
    mask = 1U << (irq % PLIC_BITS_PER_WORD);
    spin_lock_irqsave(&plic.enable_lock, flags);
    value = readl(reg);
    writel(enable ? value | mask : value & ~mask, reg);
    spin_unlock_irqrestore(&plic.enable_lock, flags);
    return 0;
}

int plic_enable(unsigned int irq)
{
    return plic_toggle(irq, true);
}

int plic_disable(unsigned int irq)
{
    return plic_toggle(irq, false);
}

unsigned int plic_claim(void)
{
    return readl(plic.claim);
}

void plic_complete(unsigned int irq)
{
    volatile uint32_t *reg = plic.enable + irq / PLIC_BITS_PER_WORD;
    uint32_t mask = 1U << (irq % PLIC_BITS_PER_WORD);
    uint32_t value;
    unsigned long flags;

    spin_lock_irqsave(&plic.enable_lock, flags);
    value = readl(reg);
    /* The PLIC ignores completion if this context has disabled the source. */
    if (!(value & mask))
        writel(value | mask, reg);
    writel(irq, plic.claim);
    if (!(value & mask))
        writel(value, reg);
    spin_unlock_irqrestore(&plic.enable_lock, flags);
}
