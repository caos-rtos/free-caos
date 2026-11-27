// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    fat_service.h
 * @brief   FAT volume service over FatFS (drive "0:", SD card via SDIO)
 *
 * @note    Every ca_fat_* operation PRINTS its outcome on stdout ("[fat] ...")
 *          and returns true on success: it is the engine behind the `fat` CLI
 *          command (EN_CLI_FAT) and is callable from an application as well.
 *
 *          FatFS is built with FF_FS_REENTRANT=0: no internal locking. Each
 *          operation takes the service lock itself; code that calls f_*()
 *          directly must hold it with ca_fat_lock()/ca_fat_unlock(). The lock
 *          is a RECURSIVE mutex, so a caller holding it may still call ca_fat_*.
 *
 *          The FatFS types (FATFS, FIL, FRESULT) stay out of this header on
 *          purpose: caos.h includes it everywhere, ff.h only where it is used.
 ******************************************************************************
 */

#ifndef FAT_SERVICE_H
#define FAT_SERVICE_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_FAT && EN_FAT_SERVICE

/* Lifecycle events, delivered WITHOUT the service lock held: the callback may
 * call ca_fat_lock() and FatFS itself (an example's stdout logger does). */
typedef enum {
    CA_FAT_EV_MOUNTED = 0,   /* volume just mounted                          */
    CA_FAT_EV_UNMOUNTING,    /* volume about to be unmounted: close files    */
    CA_FAT_EV_DESTRUCTIVE    /* fdisk/mkfs about to rewrite the card         */
} ca_fat_event_t;

typedef void (*ca_fat_event_cb_t)(ca_fat_event_t ev, const char *what);

void ca_fat_service_init(void);
void ca_fat_service_set_event_cb(ca_fat_event_cb_t cb);

void ca_fat_lock(void);
void ca_fat_unlock(void);
bool ca_fat_trylock(void);

bool        ca_fat_is_mounted(void);
const char *ca_fat_strerror(i32 fr);

/* 4 KB scratch, 32-byte aligned (SDMMC IDMA). Shared: valid only while the
 * caller holds ca_fat_lock(), and fdisk/mkfs overwrite it. */
u8 *ca_fat_work_buf(u32 *size);

bool ca_fat_fdisk(u32 mb);              /* MBR only, 0 -> 64 MB */
bool ca_fat_mkfs(void);                 /* FAT32 on the fdisk partition */
bool ca_fat_mount(void);
bool ca_fat_umount(void);
bool ca_fat_free(void);
bool ca_fat_pwd(void);
bool ca_fat_cd(const char *dir);
bool ca_fat_ls(const char *dir);        /* NULL or "" -> current directory */
bool ca_fat_mkdir(const char *dir);
bool ca_fat_rmdir(const char *dir);
bool ca_fat_touch(const char *name);
bool ca_fat_rm(const char *name);
bool ca_fat_cat(const char *name);      /* heap round trip + hexdump */

/* ---- Handle API: files and directories, NO printing ------------------------
 * For programs (the MicroPython VFS, a logger, a file server): every call
 * returns a FatFS result code, 0 = OK (ca_fat_strerror() names it), and
 * prints nothing. Paths are FatFS paths, absolute ("/dir/f.txt") or relative
 * to the volume's current directory.
 *
 * The FIL/DIR objects are allocated on the CALLER's task heap
 * (ca_memory_allocate): never on a stack, which on H7 is in DTCM where the
 * SDMMC IDMA cannot reach, and freed with the task if it dies. A handle not
 * closed loses the data not yet written; the volume stays consistent.
 * The service lock is taken per call. */

typedef struct ca_fat_file_s ca_fat_file_t;     /* opaque */
typedef struct ca_fat_dir_s  ca_fat_dir_t;      /* opaque */

#define CA_FAT_NAME_MAX     256U                /* LFN 255 + NUL */
#define CA_FAT_ATTR_RDO     0x01U
#define CA_FAT_ATTR_DIR     0x10U

/* ca_fat_fopen() mode: the FatFS FA_* values */
#define CA_FAT_O_READ       0x01U
#define CA_FAT_O_WRITE      0x02U
#define CA_FAT_O_EXISTING   0x00U               /* open, fail if missing  */
#define CA_FAT_O_NEW        0x04U               /* create, fail if exists */
#define CA_FAT_O_TRUNC      0x08U               /* create or truncate     */
#define CA_FAT_O_ALWAYS     0x10U               /* open or create         */
#define CA_FAT_O_APPEND     0x30U               /* open or create, at end */

typedef struct ca_fat_info_s {
    u32  size;
    u16  fdate;                                 /* FAT date / time        */
    u16  ftime;
    u8   attr;                                  /* CA_FAT_ATTR_*          */
    char name[CA_FAT_NAME_MAX];
} ca_fat_info_t;

i32 ca_fat_fopen(ca_fat_file_t **out, const char *path, u8 mode);
i32 ca_fat_fclose(ca_fat_file_t *f);            /* frees the handle */
i32 ca_fat_fread(ca_fat_file_t *f, void *buf, u32 len, u32 *got);
i32 ca_fat_fwrite(ca_fat_file_t *f, const void *buf, u32 len, u32 *put);
i32 ca_fat_fseek(ca_fat_file_t *f, u32 pos);
u32 ca_fat_ftell(const ca_fat_file_t *f);
u32 ca_fat_fsize(const ca_fat_file_t *f);
i32 ca_fat_fsync(ca_fat_file_t *f);

i32 ca_fat_dopen(ca_fat_dir_t **out, const char *path);
i32 ca_fat_dread(ca_fat_dir_t *d, ca_fat_info_t *info); /* name "" = end */
i32 ca_fat_dclose(ca_fat_dir_t *d);             /* frees the handle */

i32 ca_fat_pstat(const char *path, ca_fat_info_t *info);
i32 ca_fat_pmkdir(const char *path);
i32 ca_fat_punlink(const char *path);           /* file or empty dir */
i32 ca_fat_prename(const char *from, const char *to);
/* cluster size in bytes, clusters in all, clusters free */
i32 ca_fat_statvfs(u32 *cluster_bytes, u32 *total, u32 *free_clst);

/* ---- RAM disk: FatFS volume "1:" ------------------------------------------
 * A buffer of 512-byte blocks the application owns (static RAM or a shared
 * heap block) becomes a second volume, next to the SD card ("0:"). Paths
 * starting with "1:" reach it through every handle call above. The same
 * memory can be shown to a USB host as a disk (usb_msc.h: read/write =
 * ca_fat_ram_read/_write) - never to both at once: the board unmounts it
 * before the host gets it (FatFS and the host each cache the FAT).
 * ca_fat_ram_disk(NULL, 0) detaches it (unmount first). */
void ca_fat_ram_disk(u8 *buf, u32 blocks);
u32  ca_fat_ram_blocks(void);                               /* 0 = none attached */
bool ca_fat_ram_read(u32 lba, u8 *buf, u32 count);         /* MSC callback signature */
bool ca_fat_ram_write(u32 lba, const u8 *buf, u32 count);
bool ca_fat_ram_mkfs(void);                                 /* FAT, no MBR; unmounts first */
bool ca_fat_ram_mount(void);
bool ca_fat_ram_umount(void);                               /* files of "1:" must be closed */
bool ca_fat_ram_mounted(void);
i32  ca_fat_ram_statvfs(u32 *cluster_bytes, u32 *total, u32 *free_clst);

/* ---- Encrypted files (EN_FAT_CRYPT) ---------------------------------------
 * The file goes through the AES channel (aes_channel_service) synchronously:
 * on write the plaintext is fed to the channel in chunks of FAT_CRYPT_CHUNK
 * bytes and each packet it returns is appended to the file; on read each
 * packet is taken from the file and fed back to it. On disk the file is just
 * the channel packets back to back, each self-delimiting:
 *
 *     [nonce:12][len_be16:2][ciphertext:ceil16(len)][tag:16]
 *
 * Every packet carries its own GCM tag, so a modified byte or the wrong key
 * fails on the packet it hits. Packets are NOT chained: dropping or swapping
 * whole packets is not detected by the tags.
 *
 * The key is the keystore's, by index: key N of the keys the keystore service
 * has loaded - the keystore must already be unlocked.
 *
 * The channel lives for ONE call: opened on key N, used, closed, all in the
 * caller's task. Not kept between calls on purpose - the channel allocates
 * its AES contexts on the heap, heap blocks belong to the task that allocated
 * them, and every CLI command runs in a throwaway task (cli_cmd) whose blocks
 * the kernel frees when it ends. A channel opened by one command was freed
 * under the next one's feet (02/10: Invalid Free + HardFault on `fat ecat`
 * after `fat key`). */
#if EN_FAT_CRYPT && EN_AES_CHANNEL_SERVICE && EN_KEYSTORE_SERVICE

#define FAT_CRYPT_CHUNK  512U   /* plaintext bytes per packet */

/* Called once per decrypted chunk, with the service lock held. */
typedef bool (*ca_fat_read_cb_t)(const u8 *data, u32 len, void *user);

/* Create or truncate `name` and write `len` bytes of plaintext, encrypted
 * with keystore key `key_idx`. */
bool ca_fat_write_enc(u32 key_idx, const char *name, const u8 *buf, u32 len);

/* Decrypt `name` with keystore key `key_idx`, packet by packet into cb.
 * *total_out (optional) = plaintext bytes delivered. False on a bad tag
 * (wrong key or modified file), a truncated packet, or cb refusing. */
bool ca_fat_read_enc(u32 key_idx, const char *name, ca_fat_read_cb_t cb,
                     void *user, u32 *total_out);

#endif /* EN_FAT_CRYPT && EN_AES_CHANNEL_SERVICE && EN_KEYSTORE_SERVICE */

#endif /* EN_FAT && EN_FAT_SERVICE */

#endif /* FAT_SERVICE_H */
