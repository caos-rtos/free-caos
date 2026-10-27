// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 53 -- USB MSC: mass storage stick (ramdisk on heap)
 *
 * Registers the ca_usb_msc service in the ca_usb_dev_mng stack with a
 * ramdisk back-end allocated on heap (ca_memory_allocate). The device appears as
 * a removable disk; the filesystem is provided by the host (mkfs.fat), caOS exposes
 * only 512B blocks.
 *
 *   ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM);
 *   ca_usb_dev_mng_add(&s_msc.header);
 *   ca_usb_dev_mng_start();
 *
 * Hardware: H753 (USB_NUM=1), U575 (USB_NUM=0), F407 (USB_NUM=0).
 *
 * Test from PC: run the test_fat.sh script (auto-detect of the device for
 * VID caOS 0xCA01, fdisk -> sdX1, then mkfs.fat -> mount -> cp -> umount ->
 * remount + cat with persistence verification):
 *
 *   sudo ./test_fat.sh             # auto-detect
 *   sudo ./test_fat.sh /dev/sdb    # forced device
 *
 * Equivalent manual test (X = assigned letter, e.g. sdb):
 *   lsblk                          # identify the device (capacity ~128 KB)
 *   sudo mkfs.fat -F 12 /dev/sdX
 *   sudo mount /dev/sdX /mnt
 *   echo "ciao caOS" | sudo tee /mnt/test.txt
 *   sync; sudo umount /mnt
 *   sudo mount /dev/sdX /mnt; cat /mnt/test.txt; sudo umount /mnt
 *
 * REQUIREMENTS os_config.h: EN_USB 1 | EN_USB_MSC 1
 */

#include "caos.h"

/* ── Configuration for MCU family ────────────────────────────────────────── */

#if MCU_ID == 575
#define USB_NUM      0U
#define EP_BULK_MPS  64U

#elif MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */
#define EP_BULK_MPS  64U   /* 64 for OTG_FS, 512 for OTG_HS ULPI */

#elif MCU_ID == 407 || MCU_ID == 767
#define USB_NUM      0U
#define EP_BULK_MPS  64U

/* F446 is deliberately NOT here, unlike every other USB example: this one needs
 * a RAM disk and there is no room for one. Its heap is 24.7 KB (the part has no
 * CCMRAM, so TLB + PSP + MSP all sit in the same 128 KB as everything else, and
 * the USB queue pools take ~26 KB of .bss on top), against the 64 KB that the
 * BLK_COUNT comment below calls the minimum for a valid FAT12.
 * ca_memory_allocate() then returns NULL, s_disk stays NULL and every read
 * answers false - which the host reports as a medium error on sector 0
 * ("unable to read partition table"). Refusing to enumerate is better than
 * enumerating a disk that cannot be read. */

#elif MCU_ID == 563
#define USB_NUM      0U          /* USB_DRD_FS — single instance H563 */
#define EP_BULK_MPS  64U

#else
/* example not supported by hardware */
#define EX53_NO_RAM_FOR_DISK  1
#undef EN_USB
#define EN_USB 0
#endif


#if EN_USB && EN_USB_MSC

#define BLK_SIZE   512U
#if MCU_ID == 407
#define BLK_COUNT  128U          /* F407: 128 * 512 = 64 KB — minimum for valid FAT12
                                  * (RAM 128K: ramdisk 64K + USB code ~26K + stack/BSS).
                                  * U5/H7 have much more RAM → 128K. */
#else
#define BLK_COUNT  256U          /* 256 * 512 = 128 KB */
#endif
#define DISK_BYTES (BLK_SIZE * BLK_COUNT)

/* ── Ramdisk back-end (heap) ──────────────────────────────────────────────── */

static u8 *s_disk;               /* allocated in ca_os_configure */

static bool ramdisk_read(u32 lba, u8 *buf, u32 count)
{
    if (s_disk == NULL || (lba + count) > BLK_COUNT) return false;
    ca_memcpy(buf, &s_disk[lba * BLK_SIZE], count * BLK_SIZE);
    return true;
}

static bool ramdisk_write(u32 lba, const u8 *buf, u32 count)
{
    if (s_disk == NULL || (lba + count) > BLK_COUNT) return false;
    ca_memcpy(&s_disk[lba * BLK_SIZE], buf, count * BLK_SIZE);
    return true;
}

/* ── MSC class config (D20: static, persistent lifetime) ──────────────────── */

static ca_usb_msc_cfg_t s_msc = {
    .header      = { CA_USB_TYPE_MSC, &ca_usb_msc_ops },
    .mps         = 64U,
    .block_size  = BLK_SIZE,
    .block_count = BLK_COUNT,
    .read        = ramdisk_read,
    .write       = ramdisk_write,
};

/* ── Task management ──────────────────────────────────────────────────────── */

static u32 task_usb(void *arg)
{
    (void)arg;
    ca_stdout_put_n("[USB] MSC init...");
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_put_n("[USB] init FAILED");
        return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_msc.header)) {
        ca_stdout_put_n("[USB] add MSC FAILED - check EN_USB_MSC");
        return 1U;
    }
    ca_usb_dev_mng_start();
    ca_stdout_printf_n("[USB] ramdisk %lu KB ready - plug cable (mkfs.fat)",
                       (u32)(DISK_BYTES / 1024U));
    while (1) ca_os_sleep(1000U);
    return 0U;
}

/* ── Task heartbeat ───────────────────────────────────────────────────────── */

static u32 task_hb(void *arg)
{
    (void)arg;
    ca_led_init();
    while (1) {
        ca_led_toggle(1U);
        ca_os_sleep(500U);
    }
    return 0U;
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

void ca_os_configure(void)
{
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    s_disk = (u8 *)ca_memory_allocate(DISK_BYTES);
}

void ca_os_main(void)
{
    ca_os_job("usb", CA_TH_LEVEL6, task_usb, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("hb", CA_TH_LEVEL4, task_hb, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* EN_USB && EN_USB_MSC not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m53a, "=== Example 53 ===");
#ifdef EX53_NO_RAM_FOR_DISK
/* Saying "EN_USB disabled" here would be a lie: the config has EN_USB=1 and
 * EN_USB_MSC=1, and the #else above turned the flag off by hand. The reason is
 * RAM, and the console is where someone will look for it. */
PRIVATE_STRING(m53b, "no RAM for the disk on this part: needs 64 KB, heap is ~25 KB");
#else
PRIVATE_STRING(m53b, "EN_USB / EN_USB_MSC disabled");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m53a);
    PUT_N(&priv_m53b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_USB && EN_USB_MSC */
