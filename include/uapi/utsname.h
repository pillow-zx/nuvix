#ifndef _NUVIX_UAPI_UTSNAME_H
#define _NUVIX_UAPI_UTSNAME_H

/**
 * @file utsname.h
 * @brief Linux uname UAPI layout.
 */

/**
 * @def UTS_FIELD_LEN
 * @brief Size of each fixed uname string field, including NUL.
 */
#define UTS_FIELD_LEN 65

/**
 * @struct utsname
 * @brief System identity strings returned by uname().
 *
 * @par Fields
 * - @c sysname: Operating system name.
 * - @c nodename: Network node hostname.
 * - @c release: Kernel release string.
 * - @c version: Kernel version/build string.
 * - @c machine: Hardware architecture name.
 * - @c domainname: NIS/domain name field.
 */
struct utsname {
	char sysname[UTS_FIELD_LEN];
	char nodename[UTS_FIELD_LEN];
	char release[UTS_FIELD_LEN];
	char version[UTS_FIELD_LEN];
	char machine[UTS_FIELD_LEN];
	char domainname[UTS_FIELD_LEN];
};

#endif
