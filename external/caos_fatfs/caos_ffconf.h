/*
 * caOS FatFS configuration — unica sorgente di configurazione FatFS.
 * external/fatfs/source/ffconf.h fa redirect qui.
 */
#pragma once

#define FFCONF_DEF          80386   /* deve corrispondere a FF_DEFINED in ff.h */

#define FF_FS_READONLY      0
#define FF_FS_MINIMIZE      0
#define FF_USE_FIND         1   /* f_findfirst/f_findnext per ls */
#define FF_USE_MKFS         1   /* f_mkfs() per formattare */
#define FF_USE_FASTSEEK     0
#define FF_USE_EXPAND       0
#define FF_USE_CHMOD        0
#define FF_USE_LABEL        1   /* f_getlabel/f_setlabel */
#define FF_USE_FORWARD      0
#define FF_USE_STRFUNC      1   /* f_gets, f_puts, f_printf */
#define FF_PRINT_LLI        0
#define FF_PRINT_FLOAT      0
#define FF_STRF_ENCODE      0

#define FF_CODE_PAGE        850 /* Latin-1 Western Europe */

#define FF_USE_LFN          3   /* LFN heap — ff_memalloc/ff_memfree via caOS */
#define FF_MAX_LFN          255
#define FF_LFN_UNICODE      0   /* ANSI/OEM */
#define FF_LFN_BUF          255
#define FF_SFN_BUF          12
#define FF_FS_RPATH         2   /* f_chdir + f_getcwd */
#define FF_PATH_DEPTH       10

#define FF_VOLUMES          1   /* 1 volume = SD card */
#define FF_STR_VOLUME_ID    0
#define FF_VOLUME_STRS      "SD"
#define FF_MULTI_PARTITION  1   /* f_fdisk; richiede VolToPart[] in fatfs_diskio.c */

#define FF_MIN_SS           512
#define FF_MAX_SS           512

#define FF_LBA64            0
#define FF_MIN_GPT          0x10000000

#define FF_USE_TRIM         0
#define FF_FS_TINY          0
#define FF_FS_EXFAT         0
#define FF_FS_NORTC         1   /* no RTC — timestamp fisso */
#define FF_NORTC_MON        1
#define FF_NORTC_MDAY       1
#define FF_NORTC_YEAR       2024
#define FF_FS_CRTIME        0

#define FF_FS_NOFSINFO      0
#define FF_FS_LOCK          0
#define FF_FS_REENTRANT     0
