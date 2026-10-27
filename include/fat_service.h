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
