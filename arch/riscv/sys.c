#include <nuvix/errno.h>
#include <asm/sbi.h>
#include <asm/sys.h>

static int arch_reset(unsigned int type)
{
    struct sbiret ret = sbi_system_reset(type, SBI_SRST_RESET_REASON_NONE);

    switch (ret.error) {
    case SBI_ERR_NOT_SUPPORTED:
        return -ENOTSUPP;
    case SBI_ERR_INVALID_PARAM:
        return -EINVAL;
    default:
        return -EIO;
    }
}

int arch_reboot(void)
{
    return arch_reset(SBI_SRST_RESET_TYPE_COLD_REBOOT);
}

int arch_poweroff(void)
{
    return arch_reset(SBI_SRST_RESET_TYPE_SHUTDOWN);
}
