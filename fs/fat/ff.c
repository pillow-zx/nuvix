/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT Filesystem Module  R0.16                               /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2025, ChaN, all right reserved.
/
/ FatFs module is an open source software. Redistribution and use of FatFs in
/ source and binary forms, with or without modification, are permitted provided
/ that the following condition is met:
/
/ 1. Redistributions of source code must retain the above copyright notice,
/    this condition and the following disclaimer.
/
/ This software is provided by the copyright holder and contributors "AS IS"
/ and any warranties related to this software are DISCLAIMED.
/ The copyright owner or contributors be NOT LIABLE for any damages caused
/ by use of this software.
/
/----------------------------------------------------------------------------*/

#include <nuvix/string.h>
#include "ff.h"
#include "diskio.h"

#define MAX_DIR        0x200000

#define IsLower(c)     ((c) >= 'a' && (c) <= 'z')
#define IsSeparator(c) ((c) == '/' || (c) == '\\')

#define FA_SEEKEND     0x20
#define FA_MODIFIED    0x40

#define AM_VOL         0x08
#define AM_MASK        0x3F

#define NSFLAG         11
#define NS_MISSING     0x01
#define NS_LAST        0x04
#define NS_DOT         0x20
#define NS_NONAME      0x80

#define BPB_BytsPerSec 11
#define BPB_SecPerClus 13
#define BPB_RsvdSecCnt 14
#define BPB_NumFATs    16
#define BPB_TotSec32   32

#define BPB_FATSz32    36
#define BPB_RootClus32 44
#define BPB_FSInfo32   48

#define DIR_Name       0
#define DIR_Attr       11
#define DIR_CrtTime    14
#define DIR_LstAccDate 18
#define DIR_FstClusHI  20
#define DIR_ModTime    22
#define DIR_FstClusLO  26
#define DIR_FileSize   28

#define SZDIRE         32
#define DDEM           0xE5
#define RDDEM          0x05

#define FSI_LeadSig    0
#define FSI_StrucSig   484
#define FSI_Free_Count 488
#define FSI_Nxt_Free   492
#define FSI_TrailSig   508

#define ABORT(res)                                                             \
    do {                                                                       \
        fp->err = (res);                                                       \
        return (res);                                                          \
    } while (0)

struct FATFS {
    BYTE mounted, n_fats, wflag, fsi_flag;
    WORD id;
    UINT csize;
    DWORD last_clst, free_clst, cdir, n_fatent, fsize;
    LBA_t winsect, fatbase, dirbase, database;
    BYTE win[FAT_SECTOR_SIZE];
};

static FATFS Volume;
static WORD Fsid;

static WORD ld_16(const BYTE *ptr)
{
    WORD rv;

    rv = ptr[1];
    rv = rv << 8 | ptr[0];
    return rv;
}

static DWORD ld_32(const BYTE *ptr)
{
    DWORD rv;

    rv = ptr[3];
    rv = rv << 8 | ptr[2];
    rv = rv << 8 | ptr[1];
    rv = rv << 8 | ptr[0];
    return rv;
}

static void st_16(BYTE *ptr, WORD val)
{
    *ptr++ = (BYTE)val;
    val >>= 8;
    *ptr++ = (BYTE)val;
}

static void st_32(BYTE *ptr, DWORD val)
{
    *ptr++ = (BYTE)val;
    val >>= 8;
    *ptr++ = (BYTE)val;
    val >>= 8;
    *ptr++ = (BYTE)val;
    val >>= 8;
    *ptr++ = (BYTE)val;
}

static int sync_window(FATFS *fs)
{
    if (!fs->wflag)
        return 0;
    int res = disk_write(fs->win, fs->winsect, 1);
    if (res)
        return res;
    if (fs->n_fats == 2 && fs->winsect - fs->fatbase < fs->fsize) {
        res = disk_write(fs->win, fs->winsect + fs->fsize, 1);
        if (res)
            return res;
    }
    fs->wflag = 0;
    return 0;
}

static int move_window(FATFS *fs, LBA_t sect)
{
    if (sect == fs->winsect)
        return 0;
    int res = sync_window(fs);
    if (res)
        return res;
    res = disk_read(fs->win, sect, 1);
    fs->winsect = res ? UINT32_MAX : sect;
    return res;
}

static int sync_fs(FATFS *fs)
{
    int res = sync_window(fs);
    if (res != 0)
        return res;
    if (fs->fsi_flag == 1) {
        res = move_window(fs, 1);
        if (res != 0)
            return res;
        st_32(fs->win + FSI_Free_Count, fs->free_clst);
        st_32(fs->win + FSI_Nxt_Free, fs->last_clst);
        fs->wflag = 1;
        res = sync_window(fs);
        if (res != 0)
            return res;
        fs->fsi_flag = 0;
    }
    return disk_sync();
}

static LBA_t clst2sect(FATFS *fs, DWORD clst)
{
    clst -= 2;
    if (clst >= fs->n_fatent - 2)
        return 0;
    return fs->database + (LBA_t)fs->csize * clst;
}

static int get_fat(FFOBJID *obj, DWORD clst, DWORD *value)
{
    FATFS *fs = obj->fs;
    if (clst < 2 || clst >= fs->n_fatent)
        return -EUCLEAN;
    int res = move_window(fs, fs->fatbase + clst / 128);
    if (res)
        return res;
    *value = ld_32(fs->win + clst % 128 * 4) & 0x0FFFFFFF;
    if (*value == 1 || (*value >= fs->n_fatent && *value < 0x0FFFFFF8))
        return -EUCLEAN;
    return 0;
}

static int put_fat(FATFS *fs, DWORD clst, DWORD val)
{
    int res;
    BYTE *p;
    if (clst < 2 || clst >= fs->n_fatent)
        return -EUCLEAN;
    res = move_window(fs, fs->fatbase + clst / 128);
    if (res != 0)
        return res;
    p = fs->win + clst % 128 * 4;
    st_32(p, (val & 0x0FFFFFFF) | (ld_32(p) & 0xF0000000));
    fs->wflag = 1;
    return 0;
}

static int remove_chain(FFOBJID *obj, DWORD clst, DWORD pclst)
{
    FATFS *fs = obj->fs;
    int res;
    if (clst < 2 || clst >= fs->n_fatent)
        return -EUCLEAN;
    if (pclst) {
        res = put_fat(fs, pclst, 0x0FFFFFFF);
        if (res)
            return res;
    }
    do {
        DWORD next;
        res = get_fat(obj, clst, &next);
        if (res)
            return res;
        if (next < 2)
            return -EUCLEAN;
        res = put_fat(fs, clst, 0);
        if (res)
            return res;
        if (fs->free_clst < fs->n_fatent - 2)
            ++fs->free_clst;
        fs->fsi_flag |= 1;
        clst = next;
    } while (clst < fs->n_fatent);
    return 0;
}

static int create_chain(FFOBJID *obj, DWORD clst, DWORD *allocated)
{
    FATFS *fs = obj->fs;
    DWORD next = fs->last_clst, value;
    int res;
    *allocated = 0;
    if (clst) {
        res = get_fat(obj, clst, &value);
        if (res)
            return res;
        if (value < 2)
            return -EUCLEAN;
        if (value < fs->n_fatent) {
            *allocated = value;
            return 0;
        }
    }
    if (!fs->free_clst)
        return -ENOSPC;
    if (next < 2 || next >= fs->n_fatent)
        next = 1;
    for (DWORD scanned = 0; scanned < fs->n_fatent - 2; ++scanned) {
        if (++next >= fs->n_fatent)
            next = 2;
        res = get_fat(obj, next, &value);
        if (res)
            return res;
        if (value)
            continue;
        res = put_fat(fs, next, 0x0FFFFFFF);
        if (!res && clst)
            res = put_fat(fs, clst, next);
        if (res)
            return res;
        fs->last_clst = next;
        fs->fsi_flag |= 1;
        if (fs->free_clst <= fs->n_fatent - 2)
            --fs->free_clst;
        *allocated = next;
        return 0;
    }
    return -ENOSPC;
}

static int dir_clear(FATFS *fs, DWORD clst)
{
    LBA_t sector = clst2sect(fs, clst);
    int res = sync_window(fs);
    if (res)
        return res;
    if (!sector)
        return -EUCLEAN;
    fs->winsect = sector;
    memset(fs->win, 0, FAT_SECTOR_SIZE);
    for (UINT i = 0; i < fs->csize; ++i) {
        res = disk_write(fs->win, sector + i, 1);
        if (res)
            return res;
    }
    return 0;
}

static int dir_sdi(DIR *dp, DWORD ofs)
{
    FATFS *fs = dp->obj.fs;
    DWORD clst = dp->obj.sclust ? dp->obj.sclust : fs->dirbase;
    DWORD csz = fs->csize * FAT_SECTOR_SIZE;
    if (ofs >= MAX_DIR || ofs % SZDIRE)
        return -EUCLEAN;
    dp->dptr = ofs;
    while (ofs >= csz) {
        int res = get_fat(&dp->obj, clst, &clst);
        if (res)
            return res;
        if (clst < 2 || clst >= fs->n_fatent)
            return -EUCLEAN;
        ofs -= csz;
    }
    dp->clust = clst;
    dp->sect = clst2sect(fs, clst);
    if (!dp->sect)
        return -EUCLEAN;
    dp->sect += ofs / FAT_SECTOR_SIZE;
    dp->dir = fs->win + ofs % FAT_SECTOR_SIZE;
    return 0;
}

static int dir_next(DIR *dp, int stretch)
{
    FATFS *fs = dp->obj.fs;
    DWORD ofs = dp->dptr + SZDIRE, clst;
    if (!dp->sect || ofs >= MAX_DIR) {
        dp->sect = 0;
        return stretch ? -ENOSPC : 0;
    }
    if (ofs % FAT_SECTOR_SIZE == 0) {
        ++dp->sect;
        if ((ofs / FAT_SECTOR_SIZE & (fs->csize - 1)) == 0) {
            int res = get_fat(&dp->obj, dp->clust, &clst);
            if (res)
                return res;
            if (clst < 2)
                return -EUCLEAN;
            if (clst >= fs->n_fatent) {
                if (!stretch) {
                    dp->sect = 0;
                    return 0;
                }
                res = create_chain(&dp->obj, dp->clust, &clst);
                if (res)
                    return res;
                res = dir_clear(fs, clst);
                if (res)
                    return res;
            }
            dp->clust = clst;
            dp->sect = clst2sect(fs, clst);
        }
    }
    dp->dptr = ofs;
    dp->dir = fs->win + ofs % FAT_SECTOR_SIZE;
    return 0;
}

static int dir_alloc(DIR *dp, UINT n_ent)
{
    int res;
    UINT n;
    FATFS *fs = dp->obj.fs;

    res = dir_sdi(dp, 0);
    if (res == 0) {
        n = 0;
        do {
            res = move_window(fs, dp->sect);
            if (res != 0)
                break;

            if (dp->dir[DIR_Name] == DDEM || dp->dir[DIR_Name] == 0) {
                if (++n == n_ent)
                    break;
            } else {
                n = 0;
            }
            res = dir_next(dp, 1);
        } while (res == 0);
    }

    return res;
}

static DWORD ld_clust(const BYTE *dir)
{
    return (ld_16(dir + DIR_FstClusLO) | (DWORD)ld_16(dir + DIR_FstClusHI)
                                             << 16) &
           0x0FFFFFFF;
}

static void st_clust(BYTE *dir, DWORD cl)
{
    st_16(dir + DIR_FstClusLO, (WORD)cl);
    st_16(dir + DIR_FstClusHI, (WORD)(cl >> 16));
}

static int dir_read(DIR *dp)
{
    FATFS *fs = dp->obj.fs;
    while (dp->sect) {
        int res = move_window(fs, dp->sect);
        if (res)
            return res;
        if (!dp->dir[DIR_Name]) {
            dp->sect = 0;
            break;
        }
        dp->obj.attr = dp->dir[DIR_Attr] & AM_MASK;
        if (dp->dir[DIR_Name] != DDEM && dp->dir[DIR_Name] != '.' &&
            !(dp->obj.attr & AM_VOL))
            return 0;
        res = dir_next(dp, 0);
        if (res)
            return res;
    }
    return 0;
}

static int dir_find(DIR *dp)
{
    FATFS *fs = dp->obj.fs;
    int res = dir_sdi(dp, 0);
    if (res)
        return res;
    while (dp->sect) {
        res = move_window(fs, dp->sect);
        if (res)
            return res;
        if (!dp->dir[DIR_Name]) {
            dp->sect = 0;
            break;
        }
        dp->obj.attr = dp->dir[DIR_Attr] & AM_MASK;
        if (!(dp->obj.attr & AM_VOL) && !memcmp(dp->dir, dp->fn, 11))
            return 0;
        res = dir_next(dp, 0);
        if (res)
            return res;
    }
    return 0;
}

static int dir_register(DIR *dp)
{
    int res;
    FATFS *fs = dp->obj.fs;
    res = dir_alloc(dp, 1);

    if (res == 0) {
        res = move_window(fs, dp->sect);
        if (res == 0) {
            memset(dp->dir, 0, SZDIRE);
            memcpy(dp->dir + DIR_Name, dp->fn, 11);

            fs->wflag = 1;
        }
    }

    return res;
}

static int dir_remove(DIR *dp)
{
    int res;
    FATFS *fs = dp->obj.fs;

    res = move_window(fs, dp->sect);
    if (res == 0) {
        dp->dir[DIR_Name] = DDEM;
        fs->wflag = 1;
    }

    return res;
}

static void get_fileinfo(DIR *dp, FILINFO *fno)
{
    UINT si, di;

    char c;

    fno->fname[0] = 0;
    if (dp->sect == 0)
        return;

    si = di = 0;
    while (si < 11) {
        c = (char)dp->dir[si++];
        if (c == ' ')
            continue;
        if (c == RDDEM)
            c = DDEM;
        if (si == 9)
            fno->fname[di++] = '.';
        fno->fname[di++] = c;
    }
    fno->fname[di] = 0;

    fno->fattrib = dp->dir[DIR_Attr] & AM_MASK;
    fno->fsize = ld_32(dp->dir + DIR_FileSize);
    fno->ftime = ld_16(dp->dir + DIR_ModTime + 0);
    fno->fdate = ld_16(dp->dir + DIR_ModTime + 2);
}

static int create_name(DIR *dp, const char **path)
{
    const char *p = *path, *end = p;
    UINT i = 0, limit = 8;
    BYTE c;
    while (*end && !IsSeparator(*end))
        ++end;
    memset(dp->fn, ' ', 11);
    if ((end - p == 1 && p[0] == '.') ||
        (end - p == 2 && p[0] == '.' && p[1] == '.')) {
        memcpy(dp->fn, p, (size_t)(end - p));
        dp->fn[NSFLAG] = NS_DOT;
    } else {
        while (p < end) {
            c = (BYTE)*p++;
            if (c <= ' ' || c >= 0x7F || strchr("*+,:;<=>[]|\"?", c))
                return -EINVAL;
            if (c == '.') {
                if (limit == 11 || i == 0 || p == end)
                    return -EINVAL;
                i = 8;
                limit = 11;
                continue;
            }
            if (i >= limit)
                return -ENAMETOOLONG;
            dp->fn[i++] = IsLower(c) ? c - 32 : c;
        }
        if (i == 0)
            return -EINVAL;
        dp->fn[NSFLAG] = 0;
    }
    while (IsSeparator(*end))
        ++end;
    if (!*end)
        dp->fn[NSFLAG] |= NS_LAST;
    *path = end;
    return 0;
}

static int follow_path(DIR *dp, const char *path)
{
    dp->fn[NSFLAG] = 0;
    if (!path)
        return -EFAULT;
    if (!*path)
        return -ENOENT;
    FATFS *fs = dp->obj.fs;
    dp->obj.sclust = IsSeparator(*path) ? 0 : fs->cdir;
    while (IsSeparator(*path))
        ++path;
    if (!*path) {
        dp->fn[NSFLAG] = NS_NONAME;
        return dir_sdi(dp, 0);
    }
    for (;;) {
        int res = create_name(dp, &path);
        if (res)
            return res;
        BYTE flags = dp->fn[NSFLAG];
        res = dir_find(dp);
        if (res)
            return res;
        if (!dp->sect) {
            if (!(flags & NS_DOT) || dp->obj.sclust) {
                dp->fn[NSFLAG] |= NS_MISSING;
                return -ENOENT;
            }
            if (flags & NS_LAST) {
                dp->fn[NSFLAG] = NS_NONAME;
                return 0;
            }
            continue;
        }
        if (flags & NS_LAST)
            return 0;
        if (!(dp->obj.attr & AM_DIR))
            return -ENOTDIR;
        dp->obj.sclust = ld_clust(dp->dir);
    }
}

static int get_volume(FATFS **fs)
{
    *fs = &Volume;
    return Volume.mounted ? 0 : -ENODEV;
}

static int validate(FFOBJID *obj, FATFS **fs)
{
    *fs = &Volume;
    if (!obj || obj->fs != &Volume)
        return -EBADF;
    if (!Volume.mounted)
        return -ENODEV;
    if (obj->id != Volume.id)
        return -ESTALE;
    return 0;
}

int f_mount(void)
{
    FATFS *fs = &Volume;
    DWORD total, data;
    memset(fs, 0, sizeof *fs);
    fs->winsect = 0xFFFFFFFF;
    int res = disk_read(fs->win, 0, 1);
    if (res)
        return res;
    fs->csize = fs->win[BPB_SecPerClus];
    fs->n_fats = fs->win[BPB_NumFATs];
    fs->fatbase = ld_16(fs->win + BPB_RsvdSecCnt);
    fs->fsize = ld_32(fs->win + BPB_FATSz32);
    fs->dirbase = ld_32(fs->win + BPB_RootClus32);
    total = ld_32(fs->win + BPB_TotSec32);
    if (ld_16(fs->win + BPB_BytsPerSec) != FAT_SECTOR_SIZE || !fs->csize ||
        (fs->csize & (fs->csize - 1)) || fs->n_fats < 1 || fs->n_fats > 2 ||
        !fs->fsize || fs->fatbase >= total ||
        fs->fsize > (total - fs->fatbase) / fs->n_fats)
        return -EINVAL;
    fs->database = fs->fatbase + fs->n_fats * fs->fsize;
    data = total - fs->database;
    fs->n_fatent = data / fs->csize + 2;
    if (fs->dirbase < 2 || fs->dirbase >= fs->n_fatent ||
        (fs->n_fatent + 127) / 128 > fs->fsize)
        return -EINVAL;
    fs->fsi_flag = 0x80;
    fs->free_clst = fs->last_clst = 0xFFFFFFFF;
    if (ld_16(fs->win + BPB_FSInfo32) == 1) {
        res = disk_read(fs->win, 1, 1);
        if (res)
            return res;
        if (ld_32(fs->win + FSI_LeadSig) == 0x41615252 &&
            ld_32(fs->win + FSI_StrucSig) == 0x61417272 &&
            ld_32(fs->win + FSI_TrailSig) == 0xAA550000)
            fs->fsi_flag = 0;
    }
    fs->winsect = 0xFFFFFFFF;
    fs->id = ++Fsid;
    fs->mounted = 1;
    return 0;
}

int f_open(FIL *fp, const char *path, BYTE mode)
{
    FATFS *fs;
    DIR dir;
    if (!fp)
        return -EBADF;
    fp->obj.fs = 0;
    if (!(mode & (FA_READ | FA_WRITE)) || (mode & ~0x3F))
        return -EINVAL;
    if ((mode & (FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)) &&
        !(mode & FA_WRITE))
        return -EINVAL;
    int res = get_volume(&fs);
    if (res)
        return res;
    dir.obj.fs = fs;
    res = follow_path(&dir, path);
    if (!res) {
        if (mode & FA_CREATE_NEW)
            return -EEXIST;
        if ((dir.fn[NSFLAG] & NS_NONAME) || (dir.obj.attr & AM_DIR))
            return -EISDIR;
        if ((mode & FA_WRITE) && (dir.obj.attr & AM_RDO))
            return -EACCES;
    } else {
        if (res != -ENOENT ||
            (dir.fn[NSFLAG] & (NS_LAST | NS_MISSING)) !=
                (NS_LAST | NS_MISSING) ||
            !(mode & (FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)))
            return res;
        res = dir_register(&dir);
        if (res)
            return res;
        mode |= FA_CREATE_ALWAYS;
    }
    if (mode & FA_CREATE_ALWAYS) {
        DWORD clst = ld_clust(dir.dir);
        LBA_t sector = fs->winsect;
        st_32(dir.dir + DIR_CrtTime, 0x5C210000);
        st_32(dir.dir + DIR_ModTime, 0x5C210000);
        dir.dir[DIR_Attr] = AM_ARC;
        st_clust(dir.dir, 0);
        st_32(dir.dir + DIR_FileSize, 0);
        fs->wflag = 1;
        if (clst) {
            res = remove_chain(&dir.obj, clst, 0);
            if (res)
                return res;
            res = move_window(fs, sector);
            if (res)
                return res;
        }
        mode |= FA_MODIFIED;
    }
    fp->obj.fs = fs;
    fp->obj.sclust = ld_clust(dir.dir);
    fp->obj.objsize = ld_32(dir.dir + DIR_FileSize);
    fp->obj.id = fs->id;
    fp->flag = mode;
    fp->err = 0;
    fp->fptr = fp->clust = 0;
    fp->dir_sect = fs->winsect;
    fp->dir_ptr = dir.dir;
    if (mode & FA_SEEKEND) {
        res = f_lseek(fp, fp->obj.objsize);
        if (res)
            fp->obj.fs = 0;
    }
    return res;
}

int f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    FATFS *fs;
    BYTE *out = buff;
    if (!br)
        return -EFAULT;
    *br = 0;
    if (!buff && btr)
        return -EFAULT;
    int res = validate(fp ? &fp->obj : 0, &fs);
    if (res)
        return res;
    if (fp->err)
        return fp->err;
    if (!(fp->flag & FA_READ))
        return -EBADF;
    if (fp->fptr > fp->obj.objsize)
        ABORT(-EUCLEAN);
    if (btr > fp->obj.objsize - fp->fptr)
        btr = fp->obj.objsize - fp->fptr;
    while (btr) {
        if (fp->fptr % (fs->csize * FAT_SECTOR_SIZE) == 0) {
            DWORD clst = fp->obj.sclust;
            if (fp->fptr) {
                res = get_fat(&fp->obj, fp->clust, &clst);
                if (res)
                    ABORT(res);
            }
            if (clst < 2 || clst >= fs->n_fatent)
                ABORT(-EUCLEAN);
            fp->clust = clst;
        }
        LBA_t sector =
            clst2sect(fs, fp->clust) + fp->fptr / FAT_SECTOR_SIZE % fs->csize;
        UINT offset = fp->fptr % FAT_SECTOR_SIZE;
        UINT n = FAT_SECTOR_SIZE - offset;
        if (n > btr)
            n = btr;
        res = move_window(fs, sector);
        if (res)
            ABORT(res);
        memcpy(out, fs->win + offset, n);
        out += n;
        fp->fptr += n;
        *br += n;
        btr -= n;
    }
    return 0;
}

int f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    FATFS *fs;
    const BYTE *in = buff;
    int limit_error = 0;
    if (!bw)
        return -EFAULT;
    *bw = 0;
    if (!buff && btw)
        return -EFAULT;
    int res = validate(fp ? &fp->obj : 0, &fs);
    if (res)
        return res;
    if (fp->err)
        return fp->err;
    if (!(fp->flag & FA_WRITE))
        return -EBADF;
    if (btw > UINT32_MAX - fp->fptr) {
        btw = UINT32_MAX - fp->fptr;
        limit_error = -EFBIG;
    }
    while (btw) {
        if (fp->fptr % (fs->csize * FAT_SECTOR_SIZE) == 0) {
            DWORD clst = fp->obj.sclust;
            if (fp->fptr || !clst) {
                res = create_chain(&fp->obj, fp->fptr ? fp->clust : 0, &clst);
                if (res == -ENOSPC)
                    return res;
                if (res)
                    ABORT(res);
            }
            if (clst < 2 || clst >= fs->n_fatent)
                ABORT(-EUCLEAN);
            fp->clust = clst;
            if (!fp->obj.sclust) {
                fp->obj.sclust = clst;
                fp->flag |= FA_MODIFIED;
            }
        }
        LBA_t sector =
            clst2sect(fs, fp->clust) + fp->fptr / FAT_SECTOR_SIZE % fs->csize;
        UINT offset = fp->fptr % FAT_SECTOR_SIZE;
        UINT n = FAT_SECTOR_SIZE - offset;
        if (n > btw)
            n = btw;
        res = move_window(fs, sector);
        if (res)
            ABORT(res);
        memcpy(fs->win + offset, in, n);
        fs->wflag = 1;
        fp->flag |= FA_MODIFIED;
        in += n;
        fp->fptr += n;
        *bw += n;
        btw -= n;
        if (fp->fptr > fp->obj.objsize)
            fp->obj.objsize = fp->fptr;
    }
    return limit_error;
}

int f_sync(FIL *fp)
{
    FATFS *fs;
    int res = validate(fp ? &fp->obj : 0, &fs);
    if (res != 0 || !(fp->flag & FA_MODIFIED))
        return res;
    res = move_window(fs, fp->dir_sect);
    if (res != 0)
        return res;
    BYTE *dir = fp->dir_ptr;
    dir[DIR_Attr] |= AM_ARC;
    st_clust(dir, fp->obj.sclust);
    st_32(dir + DIR_FileSize, fp->obj.objsize);
    st_32(dir + DIR_ModTime, 0x5C210000);
    st_16(dir + DIR_LstAccDate, 0);
    fs->wflag = 1;
    res = sync_fs(fs);
    if (res == 0)
        fp->flag &= (BYTE)~FA_MODIFIED;
    return res;
}

int f_close(FIL *fp)
{
    int res = f_sync(fp);
    if (res == 0)
        fp->obj.fs = 0;
    return res;
}

int f_chdir(const char *path)
{
    int res;
    DIR dj;
    FATFS *fs;

    res = get_volume(&fs);
    if (res == 0) {
        dj.obj.fs = fs;

        res = follow_path(&dj, path);
        if (res == 0) {
            if (dj.fn[NSFLAG] & NS_NONAME) {
                fs->cdir = dj.obj.sclust;
            } else {
                if (dj.obj.attr & AM_DIR) {
                    fs->cdir = ld_clust(dj.dir);

                } else {
                    res = -ENOTDIR;
                }
            }
        }
    }

    return res;
}

int f_getcwd(char *buff, UINT len)
{
    int res;
    DIR dj;
    FATFS *fs;

    FILINFO fno;

    if (!buff)
        return -EFAULT;
    if (!len)
        return -EINVAL;
    buff[0] = 0;
    if (len < 2)
        return -ERANGE;
    --len;
    res = get_volume(&fs);
    if (res == 0) {
        dj.obj.fs = fs;

        char *tp = buff;
        UINT i, nl;
        DWORD ccl;

        i = len;
        dj.obj.sclust = fs->cdir;
        while ((ccl = dj.obj.sclust) != 0) {
            res = dir_sdi(&dj, 1 * SZDIRE);
            if (res != 0)
                break;
            res = move_window(fs, dj.sect);
            if (res != 0)
                break;
            dj.obj.sclust = ld_clust(dj.dir);
            res = dir_sdi(&dj, 0);
            if (res != 0)
                break;
            do {
                res = dir_read(&dj);
                if (res != 0)
                    break;
                if (!dj.sect) {
                    res = -EUCLEAN;
                    break;
                }
                if (ccl == ld_clust(dj.dir))
                    break;
                res = dir_next(&dj, 0);
            } while (res == 0);
            if (res != 0)
                break;
            get_fileinfo(&dj, &fno);
            for (nl = 0; fno.fname[nl]; nl++)
                ;
            if (i < nl + 1) {
                res = -ERANGE;
                break;
            }
            while (nl)
                buff[--i] = fno.fname[--nl];
            buff[--i] = '/';
        }
        if (res == 0) {
            if (i == len)
                buff[--i] = '/';

            if (res == 0) {
                do {
                    *tp++ = buff[i++];
                } while (i < len);
            }
        }
        *tp = 0;
    }

    return res;
}

int f_lseek(FIL *fp, FSIZE_t ofs)
{
    FATFS *fs;
    int res = validate(fp ? &fp->obj : 0, &fs);
    DWORD clst, position = 0, cluster_size;
    if (res)
        return res;
    if (fp->err)
        return fp->err;
    if (!(fp->flag & FA_WRITE) && ofs > fp->obj.objsize)
        ofs = fp->obj.objsize;
    if (!ofs) {
        fp->fptr = fp->clust = 0;
        return 0;
    }
    cluster_size = fs->csize * FAT_SECTOR_SIZE;
    clst = fp->obj.sclust;
    if (!clst) {
        res = create_chain(&fp->obj, 0, &clst);
        if (res == -ENOSPC)
            return res;
        if (res)
            ABORT(res);
        fp->obj.sclust = clst;
        fp->flag |= FA_MODIFIED;
    }
    if (clst < 2 || clst >= fs->n_fatent)
        ABORT(-EUCLEAN);
    while (ofs > cluster_size) {
        DWORD next;
        res = (fp->flag & FA_WRITE) ? create_chain(&fp->obj, clst, &next)
                                    : get_fat(&fp->obj, clst, &next);
        if (res == -ENOSPC) {
            ofs = cluster_size;
            break;
        }
        if (res)
            ABORT(res);
        if (next < 2 || next >= fs->n_fatent)
            ABORT(-EUCLEAN);
        clst = next;
        position += cluster_size;
        ofs -= cluster_size;
    }
    fp->clust = clst;
    fp->fptr = position + ofs;
    if (fp->fptr > fp->obj.objsize) {
        fp->obj.objsize = fp->fptr;
        fp->flag |= FA_MODIFIED;
    }
    return res;
}

int f_opendir(DIR *dp, const char *path)
{
    int res;
    FATFS *fs;

    if (!dp)
        return -EBADF;

    res = get_volume(&fs);
    if (res == 0) {
        dp->obj.fs = fs;

        res = follow_path(dp, path);
        if (res == 0) {
            if (!(dp->fn[NSFLAG] & NS_NONAME)) {
                if (dp->obj.attr & AM_DIR) {
                    dp->obj.sclust = ld_clust(dp->dir);

                } else {
                    res = -ENOTDIR;
                }
            }
            if (res == 0) {
                dp->obj.id = fs->id;
                res = dir_sdi(dp, 0);
            }
        }
    }
    if (res != 0)
        dp->obj.fs = 0;

    return res;
}

int f_closedir(DIR *dp)
{
    int res;
    FATFS *fs;

    res = validate(dp ? &dp->obj : 0, &fs);
    if (res == 0) {
        dp->obj.fs = 0;
    }
    return res;
}

int f_readdir(DIR *dp, FILINFO *fno)
{
    FATFS *fs;
    if (fno)
        fno->fname[0] = 0;
    int res = validate(dp ? &dp->obj : 0, &fs);
    if (res)
        return res;
    if (!fno)
        return dir_sdi(dp, 0);
    res = dir_read(dp);
    if (res)
        return res;
    get_fileinfo(dp, fno);
    if (dp->sect)
        res = dir_next(dp, 0);
    if (res)
        fno->fname[0] = 0;
    return res;
}

int f_stat(const char *path, FILINFO *fno)
{
    int res;
    DIR dj;

    res = get_volume(&dj.obj.fs);

    if (res == 0) {
        res = follow_path(&dj, path);
        if (res == 0) {
            if (dj.fn[NSFLAG] & NS_NONAME) {
                if (fno) {
                    memset(fno, 0, sizeof(*fno));
                    fno->fattrib = AM_DIR;
                    fno->fname[0] = '/';
                }
            } else {
                if (fno)
                    get_fileinfo(&dj, fno);
            }
        }
    }

    if (fno && res != 0)
        fno->fname[0] = 0;
    return res;
}

int f_getfree(DWORD *nclst)
{
    FATFS *fs;
    int res = get_volume(&fs);
    DWORD nfree = 0;
    if (!nclst)
        return -EFAULT;
    if (res != 0)
        return res;
    if (fs->free_clst > fs->n_fatent - 2) {
        for (DWORD clst = 2; clst < fs->n_fatent; ++clst) {
            res = move_window(fs, fs->fatbase + clst / 128);
            if (res != 0)
                return res;
            if (!(ld_32(fs->win + clst % 128 * 4) & 0x0FFFFFFF))
                ++nfree;
        }
        fs->free_clst = nfree;
        fs->fsi_flag |= 1;
    }
    *nclst = fs->free_clst;
    return 0;
}

int f_truncate(FIL *fp)
{
    FATFS *fs;
    int res = validate(fp ? &fp->obj : 0, &fs);
    if (res)
        return res;
    if (fp->err)
        return fp->err;
    if (!(fp->flag & FA_WRITE))
        return -EBADF;
    if (fp->fptr >= fp->obj.objsize)
        return 0;
    if (!fp->fptr) {
        res = remove_chain(&fp->obj, fp->obj.sclust, 0);
        if (!res)
            fp->obj.sclust = 0;
    } else {
        DWORD next;
        res = get_fat(&fp->obj, fp->clust, &next);
        if (!res && next < 2)
            res = -EUCLEAN;
        if (!res && next < fs->n_fatent)
            res = remove_chain(&fp->obj, next, fp->clust);
    }
    if (res)
        ABORT(res);
    fp->obj.objsize = fp->fptr;
    fp->flag |= FA_MODIFIED;
    return 0;
}

int f_unlink(const char *path)
{
    int res;
    FATFS *fs;
    DIR dj, sdj;
    DWORD dclst = 0;

    res = get_volume(&fs);
    if (res == 0) {
        dj.obj.fs = fs;

        res = follow_path(&dj, path);
        if (res == 0) {
            if (dj.fn[NSFLAG] & (NS_DOT | NS_NONAME)) {
                res = -EINVAL;
            } else if (dj.obj.attr & AM_RDO) {
                res = -EACCES;
            }
        }
        if (res == 0) {
            dclst = ld_clust(dj.dir);

            if (dj.obj.attr & AM_DIR) {
                if (dclst == fs->cdir) {
                    res = -EBUSY;
                } else

                {
                    sdj.obj.fs = fs;
                    sdj.obj.sclust = dclst;

                    res = dir_sdi(&sdj, 0);
                    if (res == 0) {
                        res = dir_read(&sdj);
                        if (!res && sdj.sect)
                            res = -ENOTEMPTY;
                    }
                }
            }
        }
        if (res == 0) {
            res = dir_remove(&dj);
            if (res == 0 && dclst != 0) {
                res = remove_chain(&dj.obj, dclst, 0);
            }
            if (res == 0)
                res = sync_fs(fs);
        }
    }

    return res;
}

int f_mkdir(const char *path)
{
    FATFS *fs;
    DIR dir;
    DWORD clst;
    int res = get_volume(&fs);
    if (res)
        return res;
    dir.obj.fs = fs;
    res = follow_path(&dir, path);
    if (!res)
        return -EEXIST;
    if (res != -ENOENT ||
        (dir.fn[NSFLAG] & (NS_LAST | NS_MISSING)) != (NS_LAST | NS_MISSING))
        return res;
    res = create_chain(&dir.obj, 0, &clst);
    if (res)
        return res;
    res = dir_clear(fs, clst);
    if (!res) {
        memset(fs->win + DIR_Name, ' ', 11);
        fs->win[DIR_Name] = '.';
        fs->win[DIR_Attr] = AM_DIR;
        st_32(fs->win + DIR_ModTime, 0x5C210000);
        st_clust(fs->win, clst);
        memcpy(fs->win + SZDIRE, fs->win, SZDIRE);
        fs->win[SZDIRE + 1] = '.';
        st_clust(fs->win + SZDIRE, dir.obj.sclust);
        fs->wflag = 1;
        res = dir_register(&dir);
    }
    if (res) {
        int cleanup = remove_chain(&dir.obj, clst, 0);
        if (!cleanup)
            cleanup = sync_fs(fs);
        return cleanup ? cleanup : res;
    }
    st_32(dir.dir + DIR_CrtTime, 0x5C210000);
    st_32(dir.dir + DIR_ModTime, 0x5C210000);
    st_clust(dir.dir, clst);
    dir.dir[DIR_Attr] = AM_DIR;
    fs->wflag = 1;
    return sync_fs(fs);
}

static int check_move(FATFS *fs, DWORD moving, DWORD parent)
{
    for (DWORD i = 0; parent && i < fs->n_fatent; ++i) {
        if (parent == moving)
            return -EINVAL;
        if (parent == fs->dirbase)
            return 0;
        LBA_t sector = clst2sect(fs, parent);
        if (!sector)
            return -EUCLEAN;
        int res = move_window(fs, sector);
        if (res != 0)
            return res;
        parent = ld_clust(fs->win + SZDIRE);
    }
    return parent ? -EUCLEAN : 0;
}

int f_rename(const char *path_old, const char *path_new)
{
    FATFS *fs;
    DIR old, dest;
    BYTE entry[SZDIRE];
    int res = get_volume(&fs);
    if (res != 0)
        return res;
    old.obj.fs = dest.obj.fs = fs;
    res = follow_path(&old, path_old);
    if (res != 0)
        return res;
    if (old.fn[NSFLAG] & (NS_DOT | NS_NONAME))
        return -EINVAL;
    memcpy(entry, old.dir, SZDIRE);
    res = follow_path(&dest, path_new);
    if (res == 0)
        return old.obj.sclust == dest.obj.sclust && old.dptr == dest.dptr
                   ? 0
                   : -EEXIST;
    if (res != -ENOENT ||
        (dest.fn[NSFLAG] & (NS_LAST | NS_MISSING)) != (NS_LAST | NS_MISSING))
        return res;
    DWORD moving = ld_clust(entry);
    if (entry[DIR_Attr] & AM_DIR) {
        res = check_move(fs, moving, dest.obj.sclust);
        if (res != 0)
            return res;
    }
    res = dir_register(&dest);
    if (res != 0)
        return res;
    memcpy(dest.dir + DIR_Attr, entry + DIR_Attr, SZDIRE - DIR_Attr);
    if (!(entry[DIR_Attr] & AM_DIR))
        dest.dir[DIR_Attr] |= AM_ARC;
    fs->wflag = 1;
    if ((entry[DIR_Attr] & AM_DIR) && old.obj.sclust != dest.obj.sclust) {
        LBA_t sector = clst2sect(fs, moving);
        if (!sector)
            return -EUCLEAN;
        res = move_window(fs, sector);
        if (res != 0)
            return res;
        st_clust(fs->win + SZDIRE, dest.obj.sclust);
        fs->wflag = 1;
    }
    res = dir_remove(&old);
    return res == 0 ? sync_fs(fs) : res;
}
