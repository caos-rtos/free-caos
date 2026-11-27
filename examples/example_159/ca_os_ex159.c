// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex159.c
 * @brief          : Example 159 — a RAM disk for the USB host and for Python
 ******************************************************************************
 * @details
 * One buffer of RAM, two owners, never at the same time:
 *
 *  - FatFS volume "1:" (ca_fat_ram_disk): the board formats it at boot
 *    and writes README.TXT on it;
 *  - USB MSC function: the PC sees it as a removable drive (read and write);
 *  - MicroPython: caos.disk moves it between the two. caos.disk.board()
 *    takes it (the host must have ejected it) and mounts it at /ram;
 *    caos.disk.host() closes /ram and shows it to the PC again.
 *
 * The PC and FatFS each cache the FAT: two owners at once would corrupt it.
 * Absent for the host, the drive answers "medium not present"; given back,
 * it tells the host the medium changed, so the host reads it afresh. The
 * host ejecting it (eject in the file manager, udisksctl power-off) hands
 * it back to the board.
 *
 *   >>> import caos, os
 *   >>> caos.disk.state()            # 'host' at boot
 *   >>> caos.disk.board()            # after ejecting it on the PC
 *   >>> os.listdir('/ram'); open('/ram/note.txt', 'w').write('from Python')
 *   >>> caos.disk.host()             # the PC sees note.txt
 *
 * MEMORY: the disk comes from the heap - 256 KB on H753 and U575, 128 KB on
 * H563 - next to the 64 KB MicroPython heap. The other parts are left out.
 *
 * CLI:
 *   py            MicroPython REPL (Ctrl-D on an empty line exits)
 *   e159 info     owner of the disk, size, ejects seen, heap free
 ******************************************************************************
 */

#include "caos.h"

#if MCU_ID == 753
#define USB_NUM      1U          /* OTG_FS, CN13 */
#define DISK_KB      256U
#elif MCU_ID == 575
#define USB_NUM      0U
#define DISK_KB      256U
#elif MCU_ID == 563
#define USB_NUM      0U          /* USB_DRD_FS */
#define DISK_KB      128U
#else
#undef  EN_USB
#define EN_USB       0           /* not built: see MEMORY above */
#endif

#if EN_USB && EN_USB_MSC && EN_FAT && EN_FAT_SERVICE && EN_PYTHON && ENABLE_CLI

#define DISK_BLOCKS  (DISK_KB * 2U)       /* 512-byte blocks */

static u8 *s_disk;
static volatile bool s_ready;             /* formatted and shown */

static ca_usb_msc_cfg_t s_msc = {
    .header      = { CA_USB_TYPE_MSC, &ca_usb_msc_ops },
    .mps         = 64U,
    .block_size  = 512U,
    .block_count = DISK_BLOCKS,
    .read        = ca_fat_ram_read,
    .write       = ca_fat_ram_write,
};

static const char s_readme[] =
    "caOS RAM disk (example 159).\r\n"
    "Eject it here, then in MicroPython: caos.disk.board() -> /ram\r\n";

/* format, README.TXT, then the USB function with the disk shown */
static u32 task_disk(void *arg)
{
    (void)arg;
    ca_usb_msc_set_ready(false);          /* the board has it while it is written */
    bool ok = ca_fat_ram_mkfs() && ca_fat_ram_mount();
    if (ok) {
        ca_fat_file_t *f = NULL;
        u32 put = 0U;
        ok = ca_fat_fopen(&f, "1:/README.TXT", CA_FAT_O_WRITE | CA_FAT_O_TRUNC) == 0
             && ca_fat_fwrite(f, s_readme, sizeof(s_readme) - 1U, &put) == 0;
        if (f != NULL) (void)ca_fat_fclose(f);
    }
    (void)ca_fat_ram_umount();
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM) || !ca_usb_dev_mng_add(&s_msc.header)) {
        ca_stdout_put_n("E159 USB init FAILED");
        return 1U;
    }
    ca_usb_dev_mng_start();
    ca_usb_msc_set_ready(true);           /* the host has it */
    s_ready = ok;
    ca_stdout_printf_n("E159 disk %u KB %s, shown to the USB host", (u32)DISK_KB,
                       ok ? "formatted with README.TXT" : "NOT formatted");
    return 0U;
}

/* Immediate: one line, runs in cli */
static bool e159_cmd(c_str *args)
{
    const char *p = (*args)->data;
    while (*p == ' ') p++;
    if (ca_strncmp(p, "info", 4) == 0) {
        const char *owner = ca_fat_ram_mounted() ? "board" : (ca_usb_msc_ready() ? "host" : "free");
        CLI_PRINTF_N("E159 disk %u KB, owner %s, ejects %u, %s, heap free %u", (u32)DISK_KB,
                     owner, ca_usb_msc_ejects(), s_ready ? "formatted" : "not ready",
                     ca_memory_get_avail_memory());
        return true;
    }
    return false;                   /* cli prints the help line */
}

static const ca_cli_cmd_t s_e159_cmds[] = {
    { "e159", "e159      : info (owner of the RAM disk, size, ejects, heap)", { e159_cmd, (ca_stack_size_t)0 } },
};

static ca_cli_table_t s_e159_table = {
    "e159", s_e159_cmds, (u16)(sizeof(s_e159_cmds) / sizeof(s_e159_cmds[0])), NULL
};

PRIVATE_STRING(nodisk, "ex159: no heap for the RAM disk");

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
        ca_cli_register_table(&s_e159_table);
    )
    ca_fat_service_init();
    s_disk = (u8 *)ca_memory_allocate_shared(DISK_BLOCKS * 512U);
    if (s_disk == NULL) PUT_N(&priv_nodisk);
    else ca_fat_ram_disk(s_disk, DISK_BLOCKS);
}

void ca_os_main(void)
{
    if (s_disk != NULL)
        ca_os_job("disk", CA_TH_LEVEL4, task_disk, NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else

PRIVATE_STRING(m159a, "=== Example 159: a RAM disk for the USB host and for Python ===");
PRIVATE_STRING(m159b, "needs USB MSC + FAT + EN_PYTHON and the RAM of H753, U575 or H563");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m159a);
    PUT_N(&priv_m159b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
