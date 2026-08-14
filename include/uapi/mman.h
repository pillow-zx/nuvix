#ifndef _NUVIX_UAPI_MMAN_H
#define _NUVIX_UAPI_MMAN_H

/**
 * @file mman.h
 * @brief Linux mmap/mprotect/mremap/msync/madvise UAPI constants.
 */

/** @def PROT_READ Mapping permits loads. */
#define PROT_READ 0x1
/** @def PROT_WRITE Mapping permits stores. */
#define PROT_WRITE 0x2
/** @def PROT_EXEC Mapping permits instruction fetch. */
#define PROT_EXEC 0x4
/** @def PROT_SEM Mapping may be used for atomic operations. */
#define PROT_SEM 0x8
/** @def PROT_NONE Mapping has no user access permissions. */
#define PROT_NONE 0x0
/** @def PROT_GROWSDOWN Extend mprotect toward lower addresses. */
#define PROT_GROWSDOWN 0x01000000
/** @def PROT_GROWSUP Extend mprotect toward higher addresses. */
#define PROT_GROWSUP 0x02000000

/** @def MAP_SHARED File mapping is shared with other mappings. */
#define MAP_SHARED 0x01
/** @def MAP_PRIVATE Mapping is private to this address space. */
#define MAP_PRIVATE 0x02
/** @def MAP_SHARED_VALIDATE Linux validated shared mapping type. */
#define MAP_SHARED_VALIDATE 0x03
/** @def MAP_FIXED Requested mmap address must be used exactly. */
#define MAP_FIXED 0x10
/** @def MAP_ANONYMOUS Mapping is not backed by a file descriptor. */
#define MAP_ANONYMOUS 0x20
/** @def MAP_TYPE Linux mapping-type mask. */
#define MAP_TYPE 0x0f
/** @def MAP_GROWSDOWN Linux stack-growth flag, unsupported by nuvix. */
#define MAP_GROWSDOWN 0x0100
/** @def MAP_DENYWRITE Historical Linux no-op compatibility flag. */
#define MAP_DENYWRITE 0x0800
/** @def MAP_EXECUTABLE Historical Linux no-op compatibility flag. */
#define MAP_EXECUTABLE 0x1000
/** @def MAP_LOCKED Linux prefault-and-lock flag, unsupported by nuvix. */
#define MAP_LOCKED 0x2000
/** @def MAP_NORESERVE Linux overcommit hint, accepted as a no-op. */
#define MAP_NORESERVE 0x4000
/** @def MAP_POPULATE Best-effort prefault of mapping pages. */
#define MAP_POPULATE 0x008000
/** @def MAP_NONBLOCK Linux MAP_POPULATE modifier, unsupported by nuvix. */
#define MAP_NONBLOCK 0x010000
/** @def MAP_STACK Linux stack-placement hint, accepted as a no-op. */
#define MAP_STACK 0x020000
/** @def MAP_HUGETLB Linux huge-page mapping flag, unsupported by nuvix. */
#define MAP_HUGETLB 0x040000
/** @def MAP_SYNC Linux DAX synchronized fault flag, unsupported by nuvix. */
#define MAP_SYNC 0x080000
/** @def MAP_FIXED_NOREPLACE Fixed mmap that must not replace an old mapping. */
#define MAP_FIXED_NOREPLACE 0x100000
/** @def MAP_UNINITIALIZED Anonymous mapping may be uninitialized. */
#define MAP_UNINITIALIZED 0x4000000

/** @def MREMAP_MAYMOVE mremap may move the mapping. */
#define MREMAP_MAYMOVE 1
/** @def MREMAP_FIXED mremap target address is supplied explicitly. */
#define MREMAP_FIXED 2
/** @def MREMAP_DONTUNMAP Linux flag reserving old mapping on move. */
#define MREMAP_DONTUNMAP 4

/** @def MS_ASYNC Request asynchronous file mapping writeback. */
#define MS_ASYNC 1
/** @def MS_INVALIDATE Request invalidation of other mappings. */
#define MS_INVALIDATE 2
/** @def MS_SYNC Request synchronous file mapping writeback. */
#define MS_SYNC 4

/** @def MLOCK_ONFAULT Lock pages only when they are faulted in. */
#define MLOCK_ONFAULT 0x01

/** @def MCL_CURRENT Lock all current mappings. */
#define MCL_CURRENT 1
/** @def MCL_FUTURE Lock all future mappings. */
#define MCL_FUTURE 2
/** @def MCL_ONFAULT Lock pages when they are faulted in. */
#define MCL_ONFAULT 4

/** @def MADV_NORMAL Default access-pattern advice. */
#define MADV_NORMAL 0
/** @def MADV_RANDOM Random access-pattern advice. */
#define MADV_RANDOM 1
/** @def MADV_SEQUENTIAL Sequential access-pattern advice. */
#define MADV_SEQUENTIAL 2
/** @def MADV_WILLNEED Prefetch/will-need advice. */
#define MADV_WILLNEED 3
/** @def MADV_DONTNEED Discard resident pages when supported. */
#define MADV_DONTNEED 4
/** @def MADV_FREE Lazy free advice. */
#define MADV_FREE 8
/** @def MADV_REMOVE Remove backing store for a range. */
#define MADV_REMOVE 9
#define MADV_DONTFORK 10
#define MADV_DOFORK 11
#define MADV_MERGEABLE 12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE 14
#define MADV_NOHUGEPAGE 15
#define MADV_DONTDUMP 16
#define MADV_DODUMP 17
#define MADV_WIPEONFORK 18
#define MADV_KEEPONFORK 19
#define MADV_COLD 20
#define MADV_PAGEOUT 21
#define MADV_POPULATE_READ 22
#define MADV_POPULATE_WRITE 23
#define MADV_DONTNEED_LOCKED 24
#define MADV_COLLAPSE 25
#define MADV_HWPOISON 100
#define MADV_SOFT_OFFLINE 101

#define MAP_FILE 0
#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE 0x2
#define PKEY_ACCESS_MASK (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE)

#endif
