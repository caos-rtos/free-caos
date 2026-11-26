// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex138.c
 * @brief          : Example 138 — framebuffer driver (first step of VNC)
 ******************************************************************************
 * @details
 * Creates one L8 framebuffer at boot, sized for the RAM of the part, and
 * exposes the driver on the CLI. No network yet: the VNC service will show
 * this same framebuffer (vnc_add.md).
 *
 *   MCU                 framebuffer   heap
 *   H753 H747 U575      640 x 480     300 KB
 *   H563 F767 F746      480 x 320     150 KB
 *   H773                320 x 240      75 KB
 *   F439                320 x 200      63 KB
 *   F407 F446           200 x 100      20 KB
 *   F103                none (20 KB of RAM): CA_EXAMPLE_UNSUPPORTED
 *
 *   fb info                     geometry, owner, palette generation
 *   fb clear <c>                whole framebuffer to color index c
 *   fb fill <x> <y> <w> <h> <c> rectangle, clipped; prints pixels written
 *   fb pixel <x> <y> [c]        read, or write then read back
 *   fb pal <i> <rrggbb>         one palette entry (hex)
 *   fb crc                      FNV-1a 32 of the pixels (to compare with VNC)
 *   fb owner start|stop         a task takes the framebuffer / that task ends
 *                               (the framebuffer dies with it: 'fb new')
 *   fb new | fb free            recreate / destroy the boot framebuffer
 *   fb test                     self-test of the driver:
 *                               "FB TEST PASS (n/n)" / "FB TEST FAIL (n/n)"
 ******************************************************************************
 */

#include "caos.h"

#if EN_FB

#if CAOS_FREE && (MCU_ID == 753 || MCU_ID == 575)
/* FREE edition: the heap is capped at half (244 KB H753, 302 KB U575, 19/11):
 * 640x480 L8 (301 KB) does not fit */
#define FB_WIDTH 480
#define FB_HEIGHT 320
#elif MCU_ID == 753 || MCU_ID == 747 || MCU_ID == 575
#define FB_WIDTH 640
#define FB_HEIGHT 480
#elif MCU_ID == 563 || MCU_ID == 767 || MCU_ID == 746
#define FB_WIDTH 480
#define FB_HEIGHT 320
#elif MCU_ID == 773
#define FB_WIDTH 320
#define FB_HEIGHT 240
#elif MCU_ID == 439
#define FB_WIDTH 320
#define FB_HEIGHT 200
#elif MCU_ID == 407 || MCU_ID == 446
#define FB_WIDTH 200
#define FB_HEIGHT 100
#else
#define FB_WIDTH 0               /* F103: 20 KB of RAM, 200x100 does not fit */
#define FB_HEIGHT 0
#endif

static ca_fb_t s_fb = CA_FB_NONE;

/* ------------------------------------------------------- owner task */

#define OWN_RUN      0U
#define OWN_EXIT     1U
#define OWN_DESTROY  2U

static volatile u32 s_own_cmd;
static volatile i32 s_own_res;          /* -1 pending, 0 take failed, 1 taken,
                                           2 destroyed, 3 destroy refused */
static ca_task_handle_t s_own = CA_TASK_HANDLE_NONE;

static u32 task_owner(void *arg)
{
    const ca_fb_t fb = (ca_fb_t)(uintptr_t)arg;
    s_own_res = ca_fb_take(fb) ? 1 : 0;
    while (s_own_cmd == OWN_RUN) ca_os_sleep(5U);
    if (s_own_cmd == OWN_DESTROY) s_own_res = ca_fb_destroy(fb) ? 2 : 3;
    return 0U;
}

/* Start an owner of fb and wait for its take. false if it did not start. */
static bool owner_start(ca_fb_t fb)
{
    s_own_cmd = OWN_RUN;
    s_own_res = -1;
    s_own = ca_os_job("fb_owner", CA_TH_LEVEL2, task_owner, (void *)(uintptr_t)fb,
                      STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    if (s_own == CA_TASK_HANDLE_NONE) return false;
    for (u32 i = 0U; i < 200U && s_own_res < 0; i++) ca_os_sleep(5U);
    return s_own_res >= 0;
}

/* Tell the owner to end (cmd) and wait until it is gone */
static bool owner_stop(u32 cmd)
{
    if (s_own == CA_TASK_HANDLE_NONE) return false;
    s_own_cmd = cmd;
    for (u32 i = 0U; i < 200U; i++) {
        if (ca_os_thid_from_handle(s_own) == (u32)-1) { s_own = CA_TASK_HANDLE_NONE; return true; }
        ca_os_sleep(5U);
    }
    return false;
}

/* ------------------------------------------------------- helpers */

static u32 fb_crc(ca_fb_t fb)
{
    ca_fb_info_t in;
    if (!ca_fb_info(fb, &in)) return 0U;
    u32 h = 2166136261U;
    for (u32 i = 0U; i < in.size; i++) { h ^= in.base[i]; h *= 16777619U; }
    return h;
}

/* Count the pixels of rectangle [x0,x1)x[y0,y1) equal to c, and of the rest
   equal to bg: true if the whole framebuffer matches */
static bool fb_match(ca_fb_t fb, u32 x0, u32 y0, u32 x1, u32 y1, u8 c, u8 bg)
{
    ca_fb_info_t in;
    if (!ca_fb_info(fb, &in)) return false;
    for (u32 y = 0U; y < in.height; y++) {
        const u8 *row = in.base + y * in.stride;
        for (u32 x = 0U; x < in.width; x++) {
            const bool inside = x >= x0 && x < x1 && y >= y0 && y < y1;
            if (row[x] != (inside ? c : bg)) return false;
        }
    }
    return true;
}

/* The heap returns to `level` within 500 ms (a dead task's blocks and stack
   are released by the scheduler, not at once) */
static bool heap_back(u32 level)
{
    for (u32 i = 0U; i < 100U; i++) {
        if (ca_memory_get_avail_memory() == level) return true;
        ca_os_sleep(5U);
    }
    return false;
}

/* ------------------------------------------------------- self-test */

static u32 s_pass, s_total;

static void check(bool ok, const char *name)
{
    s_total++;
    if (ok) s_pass++;
    CLI_PRINTF_N("FB   %s %s", ok ? "ok  " : "FAIL", name);
}

static void ex138_test(void)
{
    s_pass = 0U; s_total = 0U;
    ca_fb_info_t in;

    if (ca_fb_info(s_fb, &in) == false) {
        s_fb = ca_fb_create(FB_WIDTH, FB_HEIGHT, CA_FB_FMT_L8, 1U);
    }
    const bool have = ca_fb_info(s_fb, &in);
    check(have, "boot framebuffer valid");
    if (!have) { CLI_PRINTF_N("FB TEST FAIL (%u/%u)", s_pass, s_total); return; }
    const i32 W = in.width, H = in.height;

    /* geometry and defaults */
    check(W == FB_WIDTH && H == FB_HEIGHT && in.stride == (u32)W && in.size == (u32)(W * H) &&
          in.bpp == 8U && in.buffers == 1U && in.fmt == CA_FB_FMT_L8, "geometry");
    check(in.base == ca_fb_get(s_fb), "get == info.base");
    check(in.owner == CA_TASK_HANDLE_NONE, "shared at creation");
    bool gray = true;
    for (u32 i = 0U; i < CA_FB_PALETTE_SIZE; i++) gray = gray && in.palette[i] == i * 0x010101U;
    check(gray, "default palette gray ramp");

    /* clear and fill */
    check(ca_fb_clear(s_fb, 0x5AU) == W * H, "clear returns w*h");
    check(fb_match(s_fb, 0U, 0U, 0U, 0U, 0U, 0x5AU), "clear writes every pixel");
    check(ca_fb_fill_rect(s_fb, 10, 20, 30, 40, 7U) == 1200, "fill inside returns w*h");
    check(fb_match(s_fb, 10U, 20U, 40U, 60U, 7U, 0x5AU), "fill inside, border untouched");

    /* clipping */
    (void)ca_fb_clear(s_fb, 0U);
    check(ca_fb_fill_rect(s_fb, -5, -5, 10, 10, 3U) == 25, "clip top-left = 25");
    check(fb_match(s_fb, 0U, 0U, 5U, 5U, 3U, 0U), "clip top-left pixels");
    (void)ca_fb_clear(s_fb, 0U);
    check(ca_fb_fill_rect(s_fb, W - 3, H - 2, 10, 10, 4U) == 6, "clip bottom-right = 6");
    check(fb_match(s_fb, (u32)W - 3U, (u32)H - 2U, (u32)W, (u32)H, 4U, 0U), "clip bottom-right pixels");
    check(ca_fb_fill_rect(s_fb, W, 0, 5, 5, 9U) == 0 &&
          ca_fb_fill_rect(s_fb, -100, -100, 50, 50, 9U) == 0 &&
          ca_fb_fill_rect(s_fb, 0, H, 5, 5, 9U) == 0, "fully outside = 0");
    check(ca_fb_fill_rect(s_fb, 1, 1, 0, 5, 9U) == 0 &&
          ca_fb_fill_rect(s_fb, 1, 1, 5, -1, 9U) == 0, "w or h <= 0 = 0");
    check(ca_fb_fill_rect(s_fb, -2147483647 - 1, -2147483647 - 1, 2147483647, 2147483647, 9U) == 0,
          "i32 extremes, outside = 0");
    check(ca_fb_fill_rect(s_fb, -1000, -1000, 2147483647, 2147483647, 1U) == W * H,
          "huge rect clipped to w*h");
    check(fb_match(s_fb, 0U, 0U, 0U, 0U, 0U, 1U), "huge rect pixels");

    /* pixels */
    check(ca_fb_set_pixel(s_fb, 0, 0, 9U) == 1 && ca_fb_get_pixel(s_fb, 0, 0) == 9, "pixel (0,0)");
    check(ca_fb_set_pixel(s_fb, W - 1, H - 1, 200U) == 1 && ca_fb_get_pixel(s_fb, W - 1, H - 1) == 200,
          "pixel (w-1,h-1)");
    check(ca_fb_set_pixel(s_fb, W, 0, 9U) == 0 && ca_fb_set_pixel(s_fb, 0, -1, 9U) == 0, "pixel outside = 0");
    check(ca_fb_get_pixel(s_fb, -1, 0) == -1 && ca_fb_get_pixel(s_fb, 0, H) == -1, "get outside = -1");

    /* palette */
    static const u32 pal[6] = { 0xFF0000U, 0x00FF00U, 0x0000FFU, 0xFFFFFFU, 0x123456U, 0xAB654321U };
    const u32 g0 = in.palette_gen;
    check(ca_fb_set_palette(s_fb, 250U, 6U, pal), "palette 250..255");
    (void)ca_fb_info(s_fb, &in);
    check(in.palette_gen == g0 + 1U && in.palette[250] == 0xFF0000U && in.palette[255] == 0x654321U,
          "palette gen +1, entries masked");
    check(!ca_fb_set_palette(s_fb, 250U, 7U, pal) && !ca_fb_set_palette(s_fb, 0U, 1U, NULL),
          "palette out of range refused");

    /* bad arguments and slots */
    check(ca_fb_create(0U, 10U, CA_FB_FMT_L8, 1U) == CA_FB_NONE &&
          ca_fb_create(10U, 0U, CA_FB_FMT_L8, 1U) == CA_FB_NONE &&
          ca_fb_create(10U, 10U, CA_FB_FMT_L8, 0U) == CA_FB_NONE &&
          ca_fb_create(10U, 10U, CA_FB_FMT_L8, 3U) == CA_FB_NONE &&
          /* the first value past the last format (1 is RGB565 since 17/11) */
          ca_fb_create(10U, 10U, (ca_fb_fmt_t)(CA_FB_FMT_RGB565 + 1), 1U) == CA_FB_NONE,
          "bad create refused");
    check(ca_fb_get(CA_FB_NONE) == NULL && ca_fb_fill_rect(0x12345678U, 0, 0, 1, 1, 0U) == -1,
          "invalid handle refused");

    const u32 heap0 = ca_memory_get_avail_memory();
    const ca_fb_t t = ca_fb_create(32U, 16U, CA_FB_FMT_L8, 2U);
    check(t != CA_FB_NONE, "second framebuffer");

    /* double buffer: drawing goes to A, the reader sees B only after a
     * complete frame (present), at most every FB_COPY_MS */
    check(ca_fb_info(t, &in) && in.buffers == 2U && in.copies == 0U && in.base != ca_fb_get(t),
          "double buffer: 2 buffers, the reader's base is not A");
    const u8 *shown = in.base;
    (void)ca_fb_fill_rect(t, 0, 0, 4, 4, 7U);
    check(shown != NULL && shown[0] == 0U && ca_fb_get_pixel(t, 0, 0) == 7,
          "drawn in A, B unchanged before present");
    check(ca_fb_present(t) && shown[0] == 7U && ca_fb_info(t, &in) && in.copies == 1U,
          "present copies A to B");
    (void)ca_fb_fill_rect(t, 0, 0, 4, 4, 9U);
    (void)ca_fb_present(t);
    check(shown[0] == 7U, "second present within FB_COPY_MS: not yet");
    ca_os_sleep(FB_COPY_MS + 10U);
    {
        u32 map[CA_FB_DIRTY_WORDS(32, 16)] = { 0U };
        check(ca_fb_take_dirty(t, map, CA_FB_DIRTY_WORDS(32, 16)) && shown[0] == 9U &&
              (map[0] & 1U) != 0U && ca_fb_info(t, &in) && in.copies == 2U,
              "copied when due, change handed to the reader");
    }
    check(ca_fb_create(8U, 8U, CA_FB_FMT_L8, 1U) == CA_FB_NONE, "third refused (CA_FB_MAX 2)");
    check(ca_fb_destroy(t), "destroy shared from any task");
    check(ca_fb_get(t) == NULL && ca_fb_fill_rect(t, 0, 0, 1, 1, 0U) == -1 &&
          !ca_fb_info(t, &in) && !ca_fb_destroy(t), "stale handle refused");
    check(ca_memory_get_avail_memory() == heap0, "heap back after destroy");
    const ca_fb_t t2 = ca_fb_create(32U, 16U, CA_FB_FMT_L8, 1U);
    check(t2 != CA_FB_NONE && t2 != t, "same slot, new generation");

    /* ownership: a task takes t2, then ends -> t2 dies with it */
    check(owner_start(t2) && s_own_res == 1, "owner task takes it");
    (void)ca_fb_info(t2, &in);
    check(in.owner == s_own, "info.owner = owner task");
    check(!ca_fb_take(t2), "take refused to another task");
    check(!ca_fb_destroy(t2), "destroy refused to non-owner");
    check(ca_fb_fill_rect(t2, 0, 0, 4, 4, 5U) == 16 && ca_fb_get_pixel(t2, 3, 3) == 5,
          "non-owner draws");
    check(owner_stop(OWN_EXIT), "owner task ends");
    check(ca_fb_fill_rect(t2, 0, 0, 1, 1, 0U) == -1 && ca_fb_get(t2) == NULL,
          "dead owner: handle stale");
    check(heap_back(heap0), "dead owner: heap back");

    /* the owner destroys its own */
    const ca_fb_t t3 = ca_fb_create(32U, 16U, CA_FB_FMT_L8, 1U);
    check(t3 != CA_FB_NONE && owner_start(t3) && s_own_res == 1, "owner takes a third one");
    check(owner_stop(OWN_DESTROY) && s_own_res == 2, "owner destroys it");
    check(ca_fb_get(t3) == NULL && heap_back(heap0), "stale, heap back");

    (void)ca_fb_clear(s_fb, 0U);
    CLI_PRINTF_N("FB TEST %s (%u/%u)", s_pass == s_total ? "PASS" : "FAIL", s_pass, s_total);
}

/* ------------------------------------------------------- CLI */

#if ENABLE_CLI
/* Next integer of the command line; advances *p */
static i32 arg(const char **p, u16 base)
{
    const char *s = ca_skip_sp(*p);
    const i32 v = ca_strtol(s, base);
    while (*s != '\0' && *s != ' ') s++;
    *p = s;
    return v;
}

static void ex138_info(void)
{
    ca_fb_info_t in;
    if (!ca_fb_info(s_fb, &in)) { CLI_PUT_N("FB none ('fb new')"); return; }
    CLI_PRINTF_N("FB %ux%u L8 stride %u size %u base %08X owner %08X palette_gen %u heap free %u",
                 in.width, in.height, in.stride, in.size, (u32)in.base, in.owner,
                 in.palette_gen, ca_memory_get_avail_memory());
}

/* fb — the line arrives without the command word */
static bool fb_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "info", 4) == 0) { ex138_info(); return true; }
    if (ca_strncmp(p, "test", 4) == 0) { ex138_test(); return true; }
    if (ca_strncmp(p, "crc", 3) == 0)  { CLI_PRINTF_N("FB crc %08X", fb_crc(s_fb)); return true; }
    if (ca_strncmp(p, "clear", 5) == 0) {
        p += 5;
        CLI_PRINTF_N("FB clear: %d", ca_fb_clear(s_fb, (u8)arg(&p, 10)));
        return true;
    }
    if (ca_strncmp(p, "fill", 4) == 0) {
        p += 4;
        const i32 x = arg(&p, 10), y = arg(&p, 10), w = arg(&p, 10), h = arg(&p, 10);
        CLI_PRINTF_N("FB fill: %d", ca_fb_fill_rect(s_fb, x, y, w, h, (u8)arg(&p, 10)));
        return true;
    }
    if (ca_strncmp(p, "pixel", 5) == 0) {
        p += 5;
        const i32 x = arg(&p, 10), y = arg(&p, 10);
        if (*ca_skip_sp(p) != '\0') (void)ca_fb_set_pixel(s_fb, x, y, (u8)arg(&p, 10));
        CLI_PRINTF_N("FB pixel (%d,%d) = %d", x, y, ca_fb_get_pixel(s_fb, x, y));
        return true;
    }
    if (ca_strncmp(p, "pal", 3) == 0) {
        p += 3;
        const i32 i = arg(&p, 10);
        const u32 rgb = (u32)arg(&p, 16);
        const bool ok = i >= 0 && i < 256 && ca_fb_set_palette(s_fb, (u16)i, 1U, &rgb);
        CLI_PRINTF_N("FB pal %d = %06X: %s", i, rgb & 0xFFFFFFU, ok ? "ok" : "refused");
        return true;
    }
    if (ca_strncmp(p, "owner", 5) == 0) {
        p = ca_skip_sp(p + 5);
        if (ca_strncmp(p, "start", 5) == 0) {
            const bool ok = s_own == CA_TASK_HANDLE_NONE && owner_start(s_fb) && s_own_res == 1;
            CLI_PRINTF_N("FB owner start: %s", ok ? "taken" : "failed");
        } else {
            CLI_PRINTF_N("FB owner stop: %s", owner_stop(OWN_EXIT) ? "ended" : "no owner");
        }
        return true;
    }
    if (ca_strncmp(p, "new", 3) == 0) {
        if (!ca_fb_info(s_fb, &(ca_fb_info_t){0})) s_fb = ca_fb_create(FB_WIDTH, FB_HEIGHT, CA_FB_FMT_L8, 1U);
        ex138_info();
        return true;
    }
    if (ca_strncmp(p, "free", 4) == 0) {
        CLI_PRINTF_N("FB free: %s", ca_fb_destroy(s_fb) ? "ok" : "refused");
        return true;
    }
    return false;                   /* cli prints the help line */
}
static const ca_cli_cmd_t s_fb_cmds[] = {
    { "fb", "fb        : info | clear <c> | fill <x> <y> <w> <h> <c> | pixel <x> <y> [c] | pal <i> <rrggbb> | crc | owner start|stop | new | free | test", { fb_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_fb_table = {
    "fb", s_fb_cmds, (u16)(sizeof(s_fb_cmds) / sizeof(s_fb_cmds[0])), NULL
};
#endif

PRIVATE_STRING(why, "ex138: not enough RAM for a framebuffer (F103)");

void ca_os_configure(void)
{
    if (FB_WIDTH == 0) {
        CA_EXAMPLE_UNSUPPORTED();
        PUT_N(&priv_why);
        return;
    }
    s_fb = ca_fb_create(FB_WIDTH, FB_HEIGHT, CA_FB_FMT_L8, 1U);

    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register_table(&s_fb_table);
    )
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else  /* EN_FB */

PRIVATE_STRING(why, "ex138: built without EN_FB");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_why);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif /* EN_FB */
