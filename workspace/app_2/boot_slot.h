/*
 * boot_slot.h - app_2 bootloader: image slots and boot configuration.
 *
 * Flash map (H563 and H753, 2 MB in two 1 MB banks), see doc/app_2_user_guide.md;
 * the values live in caos/inc/boot_image.h:
 *   0x08000000  bootloader (this app), 512 KB
 *   0x08080000  slot A, 512 KB, bank 1
 *   0x08100000  slot B, 512 KB, bank 2
 *   last sector of bank 2: boot configuration
 * A slot holds a 1 KB header (tools/caos_image.py) and the image, linked to
 * start right after it.
 */
#ifndef BOOT_SLOT_H
#define BOOT_SLOT_H

#include "caos.h"

/* The map is shared with the images: caos/inc/boot_image.h */
#define BOOT_SLOT_A_BASE   CA_BOOT_SLOT_A_BASE
#define BOOT_SLOT_B_BASE   CA_BOOT_SLOT_B_BASE
#define BOOT_SLOT_LEN      CA_BOOT_SLOT_LEN
#define BOOT_HDR_SIZE      CA_BOOT_HDR_SIZE

/* 1: only images signed with the key in boot_pubkey.h start; 0: unsigned ones
 * start too (integrity only, SHA-256), but a signature present must be good */
#define BOOT_REQUIRE_SIGNED  1
#define BOOT_HDR_SIGNED      0x0001U   /* flags b0 */

#if MCU_ID == 563
#define BOOT_SUPPORTED     1
#define BOOT_CONF_ADDR     0x081FE000U     /* last 8 KB sector of bank 2 */
#elif MCU_ID == 753
#define BOOT_SUPPORTED     1
#define BOOT_CONF_ADDR     0x081E0000U     /* last 128 KB sector of bank 2 */
#else
#define BOOT_SUPPORTED     0
#endif

/* Image header, as written by tools/caos_image.py (little endian) */
typedef struct {
    u8  magic[4];          /* 'CAIM' */
    u16 hdr_version;       /* 1 */
    u16 flags;             /* b0 = signed */
    u32 image_size;        /* bytes after the header */
    u32 link_addr;         /* vector table = slot base + BOOT_HDR_SIZE */
    u32 version;
    char tag[32];
    u32 build_time;        /* epoch */
    u8  sha256[32];        /* of bytes 0..55 + the image */
    u8  signature[64];
} __attribute__((packed)) boot_hdr_t;

/* Boot configuration, in its own flash sector */
typedef struct {
    u32 magic;             /* BOOT_CONF_MAGIC */
    u32 seq;               /* written count */
    char active;           /* 'A', 'B', or 0 = none chosen */
    char pending;          /* slot on trial ('A'/'B'), 0 = none */
    char rsv[2];
    u32 check;             /* first 4 bytes of the SHA-256 of the fields above */
} boot_conf_t;

#define BOOT_CONF_MAGIC    0x43424143U     /* 'CABC' */

/* Base address of slot 'A'/'B', 0 for anything else */
u32 boot_slot_base(char slot);

/* Checks the slot: header, link address, size, SHA-256. Fills *hdr when the
 * header can be read; returns NULL if the image is valid, else the reason. */
const char *boot_slot_check(char slot, boot_hdr_t *hdr);

/* Boot configuration: false when the sector holds none (erased, corrupt) */
bool boot_conf_read(boot_conf_t *conf);
bool boot_conf_write(char active, char pending);

/* Erases the sectors of a slot */
bool boot_slot_erase(char slot);

/* Write protection of the bootloader area (0x08000000, CA_BOOT_LOADER_LEN) in the option
 * bytes: persistent across resets and power cycles. While it is on, SWD
 * cannot rewrite the bootloader either: 'boot protect off' first.
 * boot_wrp_state: 1 all protected, 0 none, -1 partly. */
int  boot_wrp_state(void);
bool boot_wrp_set(bool on);

#endif
