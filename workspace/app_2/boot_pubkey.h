/*
 * boot_pubkey.h - the Ed25519 PUBLIC key the bootloader trusts.
 * DEMO key: its private half, workspace/app_2/boot_ed25519.pem, is in the
 * repository on purpose, so anyone can sign images for this example. A real
 * product makes its own pair: tools/caos_image.py --keygen --key <pem>.
 */
#ifndef BOOT_PUBKEY_H
#define BOOT_PUBKEY_H

#define BOOT_PUBKEY_ID  "89b34951"   /* first 4 bytes of SHA-256(key), as printed by --info */

static const u8 s_boot_pubkey[32] = {
    0x36, 0x18, 0x44, 0xCB, 0x39, 0xC3, 0xE8, 0x6A,
    0xEC, 0x82, 0x9B, 0xF2, 0x61, 0xEF, 0xD5, 0xA2,
    0x93, 0xAD, 0xA0, 0xAB, 0x74, 0xA3, 0x92, 0x1D,
    0x10, 0x7B, 0xF4, 0xCE, 0x7F, 0x7A, 0xB6, 0xFA
};

#endif
