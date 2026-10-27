/*
 * boot_slot.c - app_2 bootloader: slot checks and boot configuration.
 * See boot_slot.h for the flash map.
 */
#include "caos.h"
#include "boot_slot.h"
#include "boot_pubkey.h"

#if BOOT_SUPPORTED

static const u8 s_magic[4] = { 'C', 'A', 'I', 'M' };

u32 boot_slot_base(char slot)
{
    if (slot == 'A') return BOOT_SLOT_A_BASE;
    if (slot == 'B') return BOOT_SLOT_B_BASE;
    return 0U;
}

const char *boot_slot_check(char slot, boot_hdr_t *hdr)
{
    const u32 base = boot_slot_base(slot);
    if (base == 0U) return "no such slot";
    ca_memcpy(hdr, (const void *)base, sizeof(*hdr));

    if (ca_memcmp(hdr->magic, s_magic, 4U) != 0) return "empty (no image header)";
    if (hdr->hdr_version != 1U)                  return "unknown header version";
    if (hdr->link_addr != base + BOOT_HDR_SIZE)  return "image linked for another slot";
    if (hdr->image_size == 0U ||
        hdr->image_size > BOOT_SLOT_LEN - BOOT_HDR_SIZE) return "image size out of the slot";

    /* SHA-256 of the header fields before the hash, then the image */
    ca_sha256_ctx_t ctx;
    u8 digest[32];
    ca_sha256_init(&ctx);
    ca_sha256_update(&ctx, (const u8 *)base, (u32)__builtin_offsetof(boot_hdr_t, sha256));
    ca_sha256_update(&ctx, (const u8 *)(base + BOOT_HDR_SIZE), hdr->image_size);
    ca_sha256_final(&ctx, digest);
    if (ca_memcmp(digest, hdr->sha256, 32U) != 0) return "SHA-256 mismatch";

    /* Identity: the signature covers header bytes 0..87, hash included, so the
     * image too; the flag sits in the hashed bytes and cannot be dropped */
    if (hdr->flags & BOOT_HDR_SIGNED) {
        if (!ca_ed25519_verify(hdr->signature, (const u8 *)base,
                               (u32)__builtin_offsetof(boot_hdr_t, signature), s_boot_pubkey))
            return "signature NOT valid for key " BOOT_PUBKEY_ID;
    } else if (BOOT_REQUIRE_SIGNED) {
        return "not signed (signed images only)";
    }
    return NULL;
}

static u32 conf_check(const boot_conf_t *c)
{
    u8 digest[32];
    u32 v;
    ca_sha256((const u8 *)c, (u32)__builtin_offsetof(boot_conf_t, check), digest);
    ca_memcpy(&v, digest, 4U);
    return v;
}

bool boot_conf_read(boot_conf_t *conf)
{
    ca_memcpy(conf, (const void *)BOOT_CONF_ADDR, sizeof(*conf));
    return conf->magic == BOOT_CONF_MAGIC && conf->check == conf_check(conf);
}

bool boot_conf_write(char active, char pending)
{
    boot_conf_t old, c;
    /* the buffer covers a whole flash word: 32 bytes on H7, 16 on H5 */
    static u8 buf[32] __attribute__((aligned(4)));

    ca_memset(&c, 0, sizeof(c));
    c.magic  = BOOT_CONF_MAGIC;
    c.seq    = boot_conf_read(&old) ? old.seq + 1U : 1U;
    c.active = active;
    c.pending = pending;
    c.check  = conf_check(&c);
    ca_memset(buf, 0xFF, sizeof(buf));
    ca_memcpy(buf, &c, sizeof(c));

    if (!ca_flash_unlock()) return false;
    bool ok = ca_flash_erase_sector(BOOT_CONF_ADDR) &&
              ca_flash_write(BOOT_CONF_ADDR, buf, sizeof(buf));
    ca_flash_lock();
    return ok && boot_conf_read(&old) && old.active == active && old.pending == pending;
}

bool boot_slot_erase(char slot)
{
    const u32 base = boot_slot_base(slot);
    if (base == 0U) return false;
    if (!ca_flash_unlock()) return false;
    bool ok = ca_flash_erase_range(base, BOOT_SLOT_LEN);
    ca_flash_lock();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Write protection of the bootloader area (option bytes)
 * H563: WRP1R, one bit per group of four 8 KB sectors = 32 KB
 *       (RM0481 7.4.3, 7.11.35); H753: WPSN 1, one bit per 128 KB sector
 *       (RM0433 3.4.3, 3.9.15). Active low on both. The mask covers
 *       CA_BOOT_LOADER_LEN: 512 KB = groups 0..15 (H563), sectors 0..3 (H753).
 * ------------------------------------------------------------------------- */
#define OPT_KEY1   0x08192A3BU
#define OPT_KEY2   0x4C5D6E7FU

#if MCU_ID == 563
#define WRP_UNIT   (32U * 1024U)
#define WRP_BITS   32U                         /* WRPSG1[31:0], bank 1 */
#define WRP_CUR    (FLASH->WRP1R_CUR)
#else
#define WRP_UNIT   (128U * 1024U)
#define WRP_BITS   8U                          /* WRPSN1[7:0], bank 1 */
#define WRP_CUR    (FLASH->WPSN_CUR1)
#endif
/* A customized map (boot_image.h) must keep the bootloader area made of whole
 * protection units, or 'boot protect on' would leave its tail writable */
_Static_assert(CA_BOOT_LOADER_LEN % WRP_UNIT == 0U,
               "CA_BOOT_LOADER_LEN: not a multiple of the write protection unit");
_Static_assert(CA_BOOT_LOADER_LEN / WRP_UNIT >= 1U && CA_BOOT_LOADER_LEN / WRP_UNIT < WRP_BITS,
               "CA_BOOT_LOADER_LEN: must leave room for slot A in bank 1");
#define WRP_MASK   ((1U << (CA_BOOT_LOADER_LEN / WRP_UNIT)) - 1U)

int boot_wrp_state(void)
{
    const u32 off = WRP_CUR & WRP_MASK;          /* 1 = not protected */
    return off == 0U ? 1 : (off == WRP_MASK ? 0 : -1);
}

bool boot_wrp_set(bool on)
{
    const u32 cur = WRP_CUR;
    const u32 want = on ? (cur & ~WRP_MASK) : (cur | WRP_MASK);
    bool ok;
#if MCU_ID == 563
    while (FLASH->NSSR & (FLASH_SR_BSY | FLASH_SR_WBNE | FLASH_SR_DBNE)) { }
    FLASH->NSCCR = FLASH->NSSR;                  /* clear stale error flags */
    if (FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) {
        FLASH->OPTKEYR = OPT_KEY1;
        FLASH->OPTKEYR = OPT_KEY2;
    }
    FLASH->WRP1R_PRG = want;
    FLASH->OPTCR |= FLASH_OPTCR_OPTSTART;
    while (FLASH->NSSR & FLASH_SR_BSY) { }
    ok = (FLASH->NSSR & FLASH_SR_OPTCHANGEERR) == 0U;
    FLASH->NSCCR = FLASH->NSSR;
    FLASH->OPTCR |= FLASH_OPTCR_OPTLOCK;
#else
    while (FLASH->OPTSR_CUR & FLASH_OPTSR_OPT_BUSY) { }
    if (FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) {
        FLASH->OPTKEYR = OPT_KEY1;
        FLASH->OPTKEYR = OPT_KEY2;
    }
    FLASH->WPSN_PRG1 = want;
    FLASH->OPTCR |= FLASH_OPTCR_OPTSTART;
    while (FLASH->OPTSR_CUR & FLASH_OPTSR_OPT_BUSY) { }
    ok = (FLASH->OPTSR_CUR & FLASH_OPTSR_OPTCHANGEERR) == 0U;
    if (!ok) FLASH->OPTCCR = FLASH_OPTCCR_CLR_OPTCHANGEERR;
    FLASH->OPTCR |= FLASH_OPTCR_OPTLOCK;
#endif
    return ok && boot_wrp_state() == (on ? 1 : 0);
}

#endif /* BOOT_SUPPORTED */
