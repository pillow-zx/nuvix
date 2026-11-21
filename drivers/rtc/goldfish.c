#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/layout.h>
#include <nuvix/libfdt.h>
#include <nuvix/module.h>
#include <nuvix/pgtable.h>
#include <nuvix/rtc.h>
#include <nuvix/spinlock.h>
#include <asm/io.h>

#define GOLDFISH_TIME_LOW  0x00
#define GOLDFISH_TIME_HIGH 0x04

static const volatile uint8_t *rtc_base;
static DEFINE_SPINLOCK(rtc_lock);

int rtc_read_ns(uint64_t *ns)
{
    unsigned long flags;
    uint32_t low, high;

    if (!rtc_base)
        return -ENODEV;

    spin_lock_irqsave(&rtc_lock, flags);
    low = readl(rtc_base + GOLDFISH_TIME_LOW);
    high = readl(rtc_base + GOLDFISH_TIME_HIGH);
    spin_unlock_irqrestore(&rtc_lock, flags);
    *ns = ((uint64_t)high << 32) | low;
    return 0;
}

static int goldfish_reg(const struct dtb_range *range, void *arg)
{
    struct dtb_range *reg = arg;

    if (reg->size)
        return -EINVAL;
    *reg = *range;
    return 0;
}

static int goldfish_rtc_init(void)
{
    int node = -1, ret;

    while ((node = fdt_node_offset_by_compatible(dtb.blob, node,
                                                 "google,goldfish-rtc")) >= 0) {
        struct dtb_range reg = {0};
        size_t offset;

        ret = dtb_node_available(node);
        if (ret < 0)
            return ret;
        if (!ret)
            continue;

        ret = dtb_scan_reg(node, goldfish_reg, &reg);
        if (ret)
            return ret;
        offset = reg.base & (PAGE_SIZE - 1);
        if ((offset & (sizeof(uint32_t) - 1)) ||
            reg.size < GOLDFISH_TIME_HIGH + sizeof(uint32_t) ||
            offset > PAGE_SIZE - (GOLDFISH_TIME_HIGH + sizeof(uint32_t)))
            return -EINVAL;

        ret = pgtable_map_io(NULL, RTC_VBASE, reg.base - offset, PAGE_SIZE, 0);
        if (ret)
            return ret;
        rtc_base = (const volatile uint8_t *)(RTC_VBASE + offset);
        return 0;
    }
    return node == -FDT_ERR_NOTFOUND ? 0 : -EINVAL;
}

MODULE_INIT(goldfish_rtc_init, 72);
