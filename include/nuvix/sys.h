#ifndef _NUVIX_SYS_H
#define _NUVIX_SYS_H

#define LINUX_REBOOT_MAGIC1        0xfee1deadU
#define LINUX_REBOOT_MAGIC2        672274793U
#define LINUX_REBOOT_MAGIC2A       85072278U
#define LINUX_REBOOT_MAGIC2B       369367448U
#define LINUX_REBOOT_MAGIC2C       537993216U

#define LINUX_REBOOT_CMD_RESTART   0x01234567U
#define LINUX_REBOOT_CMD_HALT      0xCDEF0123U
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDCU

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

#endif
