#ifndef _NUVIX_SYS_H
#define _NUVIX_SYS_H

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

#endif
