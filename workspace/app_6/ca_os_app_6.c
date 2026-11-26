// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * app_6 -- "Comet Guard": a fixed shooter played through VNC (H753)
 *
 * A demo of the framebuffer double buffer (ca_fb_present at the end of every
 * frame) and of the VNC service with TLS + login, driven from the viewer's
 * keyboard. Original game: a skiff at the bottom defends against a swarm of
 * geometric drones (rings, diamonds, hexagons) that sways in formation and,
 * wave after wave, sends more of them diving at the skiff and firing.
 *
 *   Left / Right   move          Space   fire (and start a game)
 *   Up             shield: 1.5 s untouchable, then 6 s to recharge
 *
 * HOW IT DRAWS
 *   - fb 320x240 RGB565, TWO buffers: the game draws in A, ca_fb_present()
 *     copies the changed tiles to B at the end of the frame, VNC reads B -
 *     the viewer never sees half a frame. FB_COPY_MS / VNC_POLL_MS 30 ms.
 *   - Black background. Every frame erases only where the objects WERE and
 *     draws them where they ARE (all erases first, then all draws): the dirty
 *     map then holds only what moves, and that is all VNC sends.
 *   - The status line (score, wave, ships) is LVGL text: LVGL runs on a
 *     second, 320x16 framebuffer of its own (ca_lvgl_start, GUI task) and
 *     never touches the game screen; the game copies the line into it only
 *     when its pixels changed.
 *   - 30 frames per second from task 'game': the frame deadline is absolute
 *     (t0 + n * 33 ms), the work of a frame is timed with the DWT.
 *
 * CONSOLE CONTROLS (the ST-LINK VCP, or any CLI): play without a viewer
 *   left [n] / right [n]   hold the key n frames (default 5 = 15 px)
 *   shield (alias up)      shield
 *   fire (alias btn1)      fire
 *   credit                 insert a coin: a new game from the title / game over
 *   They go through ca_vnc_inject_key(CA_VNC_SRC_LOCAL, ...), the same entry
 *   as the viewer's keys (one input path, whatever the source); the game task
 *   releases the key after n frames.
 *
 * CLI
 *   app6 start      a new game (as Space on the title)
 *   app6 pause      pause / resume
 *   app6 reset      back to the title, score cleared
 *   app6 stats      fps, ms per frame (avg/max), copies A->B, VNC bytes/s,
 *                   key -> frame latency (avg/max, board side), heap
 *
 * VNC: TLS 1.3 + X509Plain login (user caos / password caos-vnc-demo, the
 * identity of example 139): vncviewer -X509CA=examples/example_139/
 * tls_identity_cert.pem <ip>. Config from ex141 (caos_config --new 6 --from
 * ex141 -wp), FB_COPY_MS and VNC_POLL_MS at 30 ms.
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"
#if EN_VNC_TLS
#include "../../examples/example_139/tls_identity.h"
#include "../../examples/example_139/vnc_cred.h"
#endif

#if EN_ETH && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && \
    EN_TCP_SERVICE && EN_FB && EN_VNC_SERVICE && (MCU_ID == 753 || MCU_ID == 747)

#include "lvgl.h"

/* ============================================================================
 * Screen and timing
 * ============================================================================ */
#define SCR_W       320
#define SCR_H       240
#define HUD_H       16
#define FRAME_MS    33U

#define RGB(r, g, b)  ((u16)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | ((b) >> 3)))
#define C_BLACK   RGB(0, 0, 0)
#define C_SHOT    RGB(255, 255, 160)
#define C_EBULLET RGB(255, 80, 60)
#define C_SHIELD  RGB(80, 220, 255)

#define ETH_AF  11U
static const ca_mac_t s_mac = { .v = { 0x02U,0x00U,0x00U,0x00U,0x06U,0x01U } };
static ca_semaphore_t s_dhcp_sem;
static ca_fb_t s_fb  = CA_FB_NONE;      /* the game screen, 2 buffers, served by VNC */
static ca_fb_t s_hud = CA_FB_NONE;      /* LVGL's status line, 320 x HUD_H */

/* ============================================================================
 * Sprites: original pixel art, 16 x 12, one character per pixel
 * ============================================================================ */
#define SPR_W  16
#define SPR_H  12

typedef struct { char key; u16 rgb; } pal_entry_t;
static const pal_entry_t s_pal[] = {
    { 'C', RGB( 90, 200, 255) }, { 'B', RGB( 30,  80, 200) }, { 'W', RGB(255, 255, 255) },
    { 'Y', RGB(255, 200,  40) }, { 'O', RGB(255, 120,  20) }, { 'M', RGB(220,  60, 220) },
    { 'G', RGB( 70, 230, 110) }, { 'R', RGB(240,  50,  50) }, { 'P', RGB(255, 150, 200) },
};

static const char *const s_art_skiff[SPR_H] = {
    ".......CC.......", "......CWWC......", "......CWWC......", ".....CCCCCC.....",
    "....CCBBBBCC....", "...CCCBBBBCCC...", "..CCCCCCCCCCCC..", ".CCC.CCCCCC.CCC.",
    "CCC...CCCC...CCC", "CC....YOOY....CC", "......Y..Y......", "................",
};
/* ring drone (top row), two frames */
static const char *const s_art_ring[2][SPR_H] = {
    { ".....YYYYYY.....", "...YYYOOOOYYY...", "..YYO......OYY..", ".YYO..RRRR..OYY.",
      ".YO..RWWWWR..OY.", ".YO..RWWWWR..OY.", ".YO..RRRRRR..OY.", ".YYO..RRRR..OYY.",
      "..YYO......OYY..", "...YYYOOOOYYY...", ".....YYYYYY.....", "................" },
    { "......YYYY......", "...YYYYOOYYYY...", "..YYO......OYY..", ".YYO...RR...OYY.",
      ".YO...RWWR...OY.", "YYO..RWWWWR..OYY", "YYO..RWWWWR..OYY", ".YO...RWWR...OY.",
      ".YYO...RR...OYY.", "..YYO......OYY..", "...YYYYOOYYYY...", "......YYYY......" },
};
/* diamond drone, two frames */
static const char *const s_art_diam[2][SPR_H] = {
    { ".......GG.......", "......GGGG......", ".....GGWWGG.....", "....GGGWWGGG....",
      "...GGGGGGGGGG...", "..GGGBBGGBBGGG..", "..GGGBBGGBBGGG..", "...GGGGGGGGGG...",
      "....GGGGGGGG....", ".....GG..GG.....", "....GG....GG....", "................" },
    { ".......GG.......", "......GGGG......", ".....GGWWGG.....", "....GGGWWGGG....",
      "...GGGGGGGGGG...", "..GGGBBGGBBGGG..", "..GGGBBGGBBGGG..", "...GGGGGGGGGG...",
      "....GGGGGGGG....", "......GGGG......", ".....GG..GG.....", "................" },
};
/* hexagon drone, two frames */
static const char *const s_art_hex[2][SPR_H] = {
    { ".....MMMMMM.....", "....MMMMMMMM....", "...MMWWMMWWMM...", "..MMMWWMMWWMMM..",
      ".MMMMMMMMMMMMMM.", "MMMMPPMMMMPPMMMM", "MMMMMMMMMMMMMMMM", ".MMMMMMMMMMMMMM.",
      "..MMMMMMMMMMMM..", "...MMM....MMM...", "..MM........MM..", "................" },
    { ".....MMMMMM.....", "....MMMMMMMM....", "...MMWWMMWWMM...", "..MMMWWMMWWMMM..",
      ".MMMMMMMMMMMMMM.", "MMMMPPMMMMPPMMMM", "MMMMMMMMMMMMMMMM", ".MMMMMMMMMMMMMM.",
      "..MMMMMMMMMMMM..", "....MMM..MMM....", "....MM....MM....", "................" },
};
static const char *const s_art_boom[2][SPR_H] = {
    { "................", "......Y..Y......", "...Y...OO...Y...", "....O.OWWO.O....",
      ".....OWWWWO.....", "..YOOWWWWWWOOY..", "..YOOWWWWWWOOY..", ".....OWWWWO.....",
      "....O.OWWO.O....", "...Y...OO...Y...", "......Y..Y......", "................" },
    { "..Y....R.....Y..", "R....O....O....R", "...O...R.R...O..", ".....R.....R....",
      "..R...........R.", "O...R.......R..O", "O...R.......R..O", "..R...........R.",
      ".....R.....R....", "...O...R.R...O..", "R....O....O....R", "..Y....R.....Y.." },
};

typedef enum { SP_SKIFF = 0, SP_RING0, SP_RING1, SP_DIAM0, SP_DIAM1, SP_HEX0, SP_HEX1,
               SP_BOOM0, SP_BOOM1, SP_COUNT } sprite_id_t;
static u16 s_spr[SP_COUNT][SPR_W * SPR_H];

static u16 pal_rgb(char k)
{
    for (u32 i = 0U; i < sizeof(s_pal) / sizeof(s_pal[0]); i++)
        if (s_pal[i].key == k) return s_pal[i].rgb;
    return C_BLACK;
}

static void sprite_build(sprite_id_t id, const char *const art[SPR_H])
{
    for (u32 y = 0U; y < SPR_H; y++)
        for (u32 x = 0U; x < SPR_W; x++)
            s_spr[id][y * SPR_W + x] = pal_rgb(art[y][x]);
}

static void sprites_init(void)
{
    sprite_build(SP_SKIFF, s_art_skiff);
    sprite_build(SP_RING0, s_art_ring[0]);  sprite_build(SP_RING1, s_art_ring[1]);
    sprite_build(SP_DIAM0, s_art_diam[0]);  sprite_build(SP_DIAM1, s_art_diam[1]);
    sprite_build(SP_HEX0,  s_art_hex[0]);   sprite_build(SP_HEX1,  s_art_hex[1]);
    sprite_build(SP_BOOM0, s_art_boom[0]);  sprite_build(SP_BOOM1, s_art_boom[1]);
}

/* ============================================================================
 * Drawing: erase where it was, draw where it is
 * ============================================================================ */
typedef struct { i32 x, y, w, h; bool on; } drawn_t;

static void erase(drawn_t *d)
{
    if (d->on) (void)ca_fb_fill_rect(s_fb, d->x, d->y, d->w, d->h, C_BLACK);
    d->on = false;
}

static void put_sprite(drawn_t *d, sprite_id_t id, i32 x, i32 y)
{
    (void)ca_fb_blit(s_fb, x, y, SPR_W, SPR_H, s_spr[id], SPR_W * 2U);
    d->x = x; d->y = y; d->w = SPR_W; d->h = SPR_H; d->on = true;
}

static void put_rect(drawn_t *d, i32 x, i32 y, i32 w, i32 h, u16 c)
{
    (void)ca_fb_fill_rect(s_fb, x, y, w, h, c);
    d->x = x; d->y = y; d->w = w; d->h = h; d->on = true;
}

/* ============================================================================
 * Game state (task 'game' only, except the key bits)
 * ============================================================================ */
#define ROWS        4
#define COLS        8
#define N_EN        (ROWS * COLS)
#define CELL_X      28
#define CELL_Y      20
#define FORM_Y      (HUD_H + 10)
#define SKIFF_Y     (SCR_H - SPR_H - 6)
#define MAX_SHOTS   3
#define MAX_EBUL    8

typedef enum { EN_DEAD = 0, EN_SLOT, EN_DIVE, EN_RETURN, EN_BOOM } en_state_t;
typedef struct {
    en_state_t st;
    u8   type;              /* 0 ring, 1 diamond, 2 hexagon */
    i32  x, y;              /* Q4 fixed point while diving, pixels in slot */
    i32  vx, vy;            /* Q4 per frame */
    u32  phase;             /* dive wobble */
    u32  timer;
    drawn_t d;
} enemy_t;

typedef struct { bool on; i32 x, y; drawn_t d; } shot_t;
typedef enum { GS_TITLE = 0, GS_PLAY, GS_DYING, GS_WAVE, GS_OVER } game_state_t;

typedef struct {
    game_state_t st;
    bool   paused;
    u32    frame;
    u32    score, wave, ships;
    i32    sx;              /* skiff x */
    u32    shield_on, shield_cd;   /* frames */
    u32    invuln, timer;
    i32    fx, fdir;        /* formation sway */
    u32    alive;
    enemy_t en[N_EN];
    shot_t shot[MAX_SHOTS];
    shot_t ebul[MAX_EBUL];
    drawn_t skiff_d;
    u32    rng;
} game_t;
static game_t g;

/* Keys: written by the VNC task, read by the game task */
#define K_LEFT   0x01U
#define K_RIGHT  0x02U
#define K_UP     0x04U
#define K_FIRE   0x08U
static volatile u32 s_keys;
static volatile u32 s_fire_req;          /* edge: a Space press not yet used */
static volatile u32 s_key_ms;            /* when the last key went down */
static volatile bool s_key_pending;

/* Console keys: pressed by the CLI, released by the game task after n frames */
typedef enum { HK_LEFT = 0, HK_RIGHT, HK_UP, HK_FIRE, HK_COUNT } hold_key_t;
static const u32 s_hold_sym[HK_COUNT] = { 0xFF51U, 0xFF53U, 0xFF52U, 0x0020U };
static volatile u32 s_hold[HK_COUNT];

/* Commands from the CLI, done by the game task at the start of a frame */
typedef enum { CMD_NONE = 0, CMD_START, CMD_PAUSE, CMD_RESET } game_cmd_t;
static volatile game_cmd_t s_cmd = CMD_NONE;

/* Statistics */
typedef struct {
    u32 frames, fps, fps_frames, fps_t0;
    u32 work_us_sum, work_us_max, work_n;
    u32 lat_sum, lat_max, lat_n;
    u32 vnc_bytes0, vnc_bps, vnc_t0;
    u32 late;               /* frames that missed their deadline */
} stats_t;
static stats_t s_st;

static u32 rnd(void)
{
    g.rng = g.rng * 1664525U + 1013904223U;
    return g.rng >> 8;
}

/* integer sine, 32 steps a turn, amplitude 64 */
static const i8 s_sin32[32] = {
    0, 12, 24, 36, 45, 53, 59, 63, 64, 63, 59, 53, 45, 36, 24, 12,
    0, -12, -24, -36, -45, -53, -59, -63, -64, -63, -59, -53, -45, -36, -24, -12
};

static i32 slot_x(u32 i) { return g.fx + (i32)(i % COLS) * CELL_X; }
static i32 slot_y(u32 i) { return FORM_Y + (i32)(i / COLS) * CELL_Y; }

static sprite_id_t enemy_sprite(const enemy_t *e, u32 anim)
{
    static const sprite_id_t base[3] = { SP_RING0, SP_DIAM0, SP_HEX0 };
    return (sprite_id_t)(base[e->type] + (anim & 1U));
}

static void wave_init(void)
{
    g.fx = (SCR_W - (COLS - 1) * CELL_X - SPR_W) / 2;
    g.fdir = 1;
    g.alive = N_EN;
    for (u32 i = 0U; i < N_EN; i++) {
        enemy_t *e = &g.en[i];
        erase(&e->d);
        e->st = EN_SLOT;
        e->type = (u8)((i / COLS) == 0U ? 0U : ((i / COLS) == 1U ? 1U : 2U));
        e->x = slot_x(i); e->y = slot_y(i);
        e->timer = 0U;
    }
    for (u32 i = 0U; i < MAX_SHOTS; i++) { erase(&g.shot[i].d); g.shot[i].on = false; }
    for (u32 i = 0U; i < MAX_EBUL; i++)  { erase(&g.ebul[i].d); g.ebul[i].on = false; }
}

static void game_new(void)
{
    g.score = 0U; g.wave = 1U; g.ships = 3U;
    g.sx = (SCR_W - SPR_W) / 2;
    g.shield_on = 0U; g.shield_cd = 0U; g.invuln = 60U;
    wave_init();
    g.st = GS_PLAY;
    s_fire_req = 0U;
}

/* difficulty by wave */
static u32 dive_every(void)  { u32 f = 90U - 10U * g.wave; return f < 25U ? 25U : f; }
static u32 max_divers(void)  { u32 n = 1U + g.wave / 2U;   return n > 5U ? 5U : n; }
static i32 dive_speed(void)  { i32 v = 28 + (i32)g.wave * 4; return v > 64 ? 64 : v; }   /* Q4 */
static u32 fire_chance(void) { u32 c = 2U + g.wave * 2U;  return c > 20U ? 20U : c; }  /* per 256 */

static void start_dive(void)
{
    u32 n = 0U;
    for (u32 i = 0U; i < N_EN; i++) if (g.en[i].st == EN_DIVE || g.en[i].st == EN_RETURN) n++;
    if (n >= max_divers()) return;
    /* pick a live slot at random, prefer the lower rows */
    for (u32 tries = 0U; tries < 8U; tries++) {
        const u32 i = rnd() % N_EN;
        enemy_t *e = &g.en[i];
        if (e->st != EN_SLOT) continue;
        e->st = EN_DIVE;
        e->x = slot_x(i) << 4;
        e->y = slot_y(i) << 4;
        e->vy = dive_speed();
        const i32 dx = (g.sx - slot_x(i)) << 4;
        const i32 frames = ((SCR_H - slot_y(i)) << 4) / e->vy;
        e->vx = frames ? dx / frames : 0;
        e->phase = rnd() & 31U;
        return;
    }
}

static bool hit(i32 ax, i32 ay, i32 aw, i32 ah, i32 bx, i32 by, i32 bw, i32 bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void fire_enemy_bullet(i32 x, i32 y)
{
    for (u32 b = 0U; b < MAX_EBUL; b++) {
        if (g.ebul[b].on) continue;
        g.ebul[b].on = true; g.ebul[b].x = x; g.ebul[b].y = y;
        return;
    }
}

static void skiff_hit(void)
{
    if (g.invuln || g.shield_on) return;
    g.st = GS_DYING;
    g.timer = 45U;
}

/* One frame of logic */
static void game_step(void)
{
    const u32 keys = s_keys;
    g.frame++;

    if (g.st == GS_TITLE || g.st == GS_OVER) {
        if (s_fire_req) { s_fire_req = 0U; game_new(); }
    }

    /* formation sway: 1 px every other frame, bounce at the edges */
    if ((g.frame & 1U) == 0U) {
        g.fx += g.fdir;
        const i32 right = g.fx + (COLS - 1) * CELL_X + SPR_W;
        if (g.fx <= 4 || right >= SCR_W - 4) g.fdir = -g.fdir;
    }

    if (g.st == GS_PLAY) {
        if ((keys & K_LEFT)  && g.sx > 2)                 g.sx -= 3;
        if ((keys & K_RIGHT) && g.sx < SCR_W - SPR_W - 2) g.sx += 3;
        if ((keys & K_UP) && g.shield_cd == 0U) { g.shield_on = 45U; g.shield_cd = 180U + 45U; }
        if (s_fire_req) {
            s_fire_req = 0U;
            for (u32 s = 0U; s < MAX_SHOTS; s++) {
                if (g.shot[s].on) continue;
                g.shot[s].on = true; g.shot[s].x = g.sx + SPR_W / 2 - 1; g.shot[s].y = SKIFF_Y - 6;
                break;
            }
        }
        if (g.frame % dive_every() == 0U) start_dive();
    }
    if (g.shield_on) g.shield_on--;
    if (g.shield_cd) g.shield_cd--;
    if (g.invuln) g.invuln--;

    /* skiff shots */
    for (u32 s = 0U; s < MAX_SHOTS; s++) {
        shot_t *sh = &g.shot[s];
        if (!sh->on) continue;
        sh->y -= 6;
        if (sh->y < HUD_H) { sh->on = false; continue; }
        for (u32 i = 0U; i < N_EN; i++) {
            enemy_t *e = &g.en[i];
            if (e->st != EN_SLOT && e->st != EN_DIVE && e->st != EN_RETURN) continue;
            const i32 ex = (e->st == EN_SLOT) ? slot_x(i) : (e->x >> 4);
            const i32 ey = (e->st == EN_SLOT) ? slot_y(i) : (e->y >> 4);
            if (hit(sh->x, sh->y, 2, 6, ex + 2, ey + 1, SPR_W - 4, SPR_H - 2)) {
                static const u32 pts[3] = { 40U, 20U, 10U };
                g.score += pts[e->type] * (e->st == EN_SLOT ? 1U : 2U);
                e->st = EN_BOOM; e->timer = 10U; e->x = ex; e->y = ey;
                g.alive--;
                sh->on = false;
                break;
            }
        }
    }

    /* drones */
    for (u32 i = 0U; i < N_EN; i++) {
        enemy_t *e = &g.en[i];
        switch (e->st) {
        case EN_DIVE: {
            e->phase++;
            e->x += e->vx + ((i32)s_sin32[(e->phase / 2U) & 31U] * 3) / 8;
            e->y += e->vy;
            const i32 px = e->x >> 4, py = e->y >> 4;
            if (g.st == GS_PLAY && py > slot_y(i) + 30 && py < SKIFF_Y - 30 &&
                (rnd() & 255U) < fire_chance())
                fire_enemy_bullet(px + SPR_W / 2, py + SPR_H);
            if (g.st == GS_PLAY && hit(px + 2, py + 2, SPR_W - 4, SPR_H - 4,
                                       g.sx + 2, SKIFF_Y + 2, SPR_W - 4, SPR_H - 4)) skiff_hit();
            if (py > SCR_H) { e->st = EN_RETURN; e->y = HUD_H << 4; }   /* below the status line */
            break;
        }
        case EN_RETURN: {
            /* back into its slot from above, steering towards it */
            const i32 tx = slot_x(i) << 4, ty = slot_y(i) << 4;
            e->x += (tx - e->x) / 8;
            e->y += 24;
            if (e->y >= ty) { e->st = EN_SLOT; }
            break;
        }
        case EN_BOOM:
            if (--e->timer == 0U) e->st = EN_DEAD;
            break;
        case EN_SLOT:
            /* from wave 3 the formation fires too */
            if (g.st == GS_PLAY && g.wave >= 3U && (rnd() % 4096U) < fire_chance())
                fire_enemy_bullet(slot_x(i) + SPR_W / 2, slot_y(i) + SPR_H);
            break;
        default:
            break;
        }
    }

    /* drone bullets */
    for (u32 b = 0U; b < MAX_EBUL; b++) {
        shot_t *eb = &g.ebul[b];
        if (!eb->on) continue;
        eb->y += 4;
        if (eb->y > SCR_H) { eb->on = false; continue; }
        if (g.st == GS_PLAY && hit(eb->x, eb->y, 3, 5, g.sx + 3, SKIFF_Y + 3, SPR_W - 6, SPR_H - 5)) {
            eb->on = false;
            skiff_hit();
        }
    }

    if (g.st == GS_DYING && --g.timer == 0U) {
        if (g.ships > 1U) {
            g.ships--;
            g.st = GS_PLAY; g.invuln = 60U;
            g.sx = (SCR_W - SPR_W) / 2;
            for (u32 b = 0U; b < MAX_EBUL; b++) g.ebul[b].on = false;
        } else {
            g.ships = 0U;
            g.st = GS_OVER;
        }
    }
    if (g.st == GS_PLAY && g.alive == 0U) { g.st = GS_WAVE; g.timer = 60U; }
    if (g.st == GS_WAVE && --g.timer == 0U) { g.wave++; wave_init(); g.st = GS_PLAY; }
}

/* One frame of drawing: every erase, then every draw */
static void game_draw(void)
{
    for (u32 i = 0U; i < N_EN; i++) erase(&g.en[i].d);
    for (u32 s = 0U; s < MAX_SHOTS; s++) erase(&g.shot[s].d);
    for (u32 b = 0U; b < MAX_EBUL; b++) erase(&g.ebul[b].d);
    erase(&g.skiff_d);

    const u32 anim = g.frame / 12U;
    for (u32 i = 0U; i < N_EN; i++) {
        enemy_t *e = &g.en[i];
        switch (e->st) {
        case EN_SLOT:   put_sprite(&e->d, enemy_sprite(e, anim), slot_x(i), slot_y(i)); break;
        case EN_DIVE:
        case EN_RETURN: put_sprite(&e->d, enemy_sprite(e, anim), e->x >> 4, e->y >> 4); break;
        case EN_BOOM:   put_sprite(&e->d, e->timer > 5U ? SP_BOOM0 : SP_BOOM1, e->x, e->y); break;
        default: break;
        }
    }
    for (u32 s = 0U; s < MAX_SHOTS; s++)
        if (g.shot[s].on) put_rect(&g.shot[s].d, g.shot[s].x, g.shot[s].y, 2, 6, C_SHOT);
    for (u32 b = 0U; b < MAX_EBUL; b++)
        if (g.ebul[b].on) put_rect(&g.ebul[b].d, g.ebul[b].x, g.ebul[b].y, 3, 5, C_EBULLET);

    if (g.st == GS_PLAY || g.st == GS_WAVE) {
        /* blink while untouchable, a frame of shield colour under the shield */
        if (g.shield_on && (g.frame & 2U))
            put_rect(&g.skiff_d, g.sx - 2, SKIFF_Y - 2, SPR_W + 4, SPR_H + 4, C_SHIELD);
        else if (!g.invuln || (g.frame & 4U))
            put_sprite(&g.skiff_d, SP_SKIFF, g.sx, SKIFF_Y);
        if (g.skiff_d.on && g.skiff_d.w != SPR_W)     /* shield frame drawn: ship on top */
            (void)ca_fb_blit(s_fb, g.sx, SKIFF_Y, SPR_W, SPR_H, s_spr[SP_SKIFF], SPR_W * 2U);
    } else if (g.st == GS_DYING) {
        put_sprite(&g.skiff_d, (g.timer / 6U) & 1U ? SP_BOOM0 : SP_BOOM1, g.sx, SKIFF_Y);
    }
}

/* ============================================================================
 * Status line: LVGL text on its own small framebuffer
 * ============================================================================ */
static lv_obj_t *s_hud_label;
static char      s_hud_text[64];         /* written by 'game', read by the GUI task */
static char      s_hud_shown[64];
static u16       s_hud_copy[SCR_W * HUD_H];

static void hud_build(void *arg)
{
    (void)arg;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);                   /* no scrollbar on a long text */
    s_hud_label = lv_label_create(scr);
    lv_obj_set_width(s_hud_label, SCR_W - 8);
    lv_label_set_long_mode(s_hud_label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(s_hud_label, lv_color_make(255, 220, 120), 0);
    lv_obj_set_style_text_font(s_hud_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_hud_label, "COMET GUARD");
    lv_obj_align(s_hud_label, LV_ALIGN_LEFT_MID, 4, 0);
}

static void hud_apply(void *arg)
{
    (void)arg;
    char t[64];
    ca_enter_critical();
    ca_memcpy(t, s_hud_text, sizeof t);
    ca_exit_critical();
    lv_label_set_text(s_hud_label, t);
}

static void hud_update(void)
{
    static const char *const msg[] = {
        "COMET GUARD - SPACE TO PLAY", "", "", "WAVE CLEARED", "GAME OVER"
    };
    char t[64];
    if (g.st == GS_PLAY || g.st == GS_DYING || g.st == GS_WAVE) {
        ca_snprintf(t, sizeof t, "SCORE %06u  WAVE %u  SHIPS %u%s%s",
                    g.score, g.wave, g.ships,
                    g.st == GS_WAVE ? "  CLEAR" : "",
                    g.paused ? "  PAUSE" : "");
    } else if (g.st == GS_OVER) {
        ca_snprintf(t, sizeof t, "%s  SCORE %06u  SPACE", msg[g.st], g.score);
    } else {
        ca_snprintf(t, sizeof t, "%s", msg[g.st]);
    }
    if (ca_strcmp(t, s_hud_shown) == 0) return;
    ca_enter_critical();
    ca_memcpy(s_hud_text, t, sizeof t);
    ca_exit_critical();
    /* shown only once LVGL took the request: a full queue retries next frame */
    if (ca_lvgl_call(hud_apply, NULL)) ca_memcpy(s_hud_shown, t, sizeof t);
}

/* the line LVGL drew, into the game screen when its pixels changed */
static void hud_copy(void)
{
    const u8 *src = ca_fb_get(s_hud);
    if (src == NULL) return;
    if (ca_memcmp(src, s_hud_copy, sizeof s_hud_copy) == 0) return;
    ca_memcpy(s_hud_copy, src, sizeof s_hud_copy);
    (void)ca_fb_blit(s_fb, 0, 0, SCR_W, HUD_H, s_hud_copy, SCR_W * 2U);
}

/* ============================================================================
 * VNC input
 * ============================================================================ */
#define XK_LEFT   0xFF51U
#define XK_UP     0xFF52U
#define XK_RIGHT  0xFF53U
#define XK_SPACE  0x0020U

static void on_key(ca_vnc_src_t src, bool down, u32 keysym)
{
    (void)src;
    u32 bit = 0U;
    if (keysym == XK_LEFT)       bit = K_LEFT;
    else if (keysym == XK_RIGHT) bit = K_RIGHT;
    else if (keysym == XK_UP)    bit = K_UP;
    else if (keysym == XK_SPACE) bit = K_FIRE;
    if (bit == 0U) return;
    if (down) {
        s_keys |= bit;
        if (bit == K_FIRE) s_fire_req = 1U;
        s_key_ms = ca_os_tick_ms();
        s_key_pending = true;
    } else {
        s_keys &= ~bit;
    }
}

/* A key from the console: the injection entry of the VNC service, or the
 * callback itself while the service is not up yet (no address) */
static void local_key(bool down, u32 keysym)
{
    if (!ca_vnc_inject_key(CA_VNC_SRC_LOCAL, down, keysym)) on_key(CA_VNC_SRC_LOCAL, down, keysym);
}

static void hold_release(void)
{
    for (u32 k = 0U; k < HK_COUNT; k++) {
        if (s_hold[k] == 0U) continue;
        if (--s_hold[k] == 0U) local_key(false, s_hold_sym[k]);
    }
}

static void on_pointer(ca_vnc_src_t src, u8 buttons, u16 x, u16 y)
{
    (void)src; (void)buttons; (void)x; (void)y;     /* keyboard game */
}

/* ============================================================================
 * Task 'game': 30 frames per second
 * ============================================================================ */
static u32 cyc_to_us(u32 c) { return c / (u32)(SYSCLK_FREQ_HZ / 1000000U); }

static void stats_frame(u32 work_us, u32 now)
{
    s_st.frames++;
    s_st.work_us_sum += work_us; s_st.work_n++;
    if (work_us > s_st.work_us_max) s_st.work_us_max = work_us;
    s_st.fps_frames++;
    if (now - s_st.fps_t0 >= 1000U) {
        s_st.fps = s_st.fps_frames * 1000U / (now - s_st.fps_t0);
        s_st.fps_frames = 0U; s_st.fps_t0 = now;
        ca_vnc_stats_t vs;
        ca_vnc_service_stats(&vs);
        const u32 dt = now - s_st.vnc_t0;
        if (s_st.vnc_t0 != 0U && dt) s_st.vnc_bps = (vs.bytes_tx - s_st.vnc_bytes0) * 1000U / dt;
        s_st.vnc_bytes0 = vs.bytes_tx; s_st.vnc_t0 = now;
    }
    if (s_key_pending) {
        /* the key was in the state this frame used, and the frame is out */
        const u32 lat = now - s_key_ms;
        s_key_pending = false;
        s_st.lat_sum += lat; s_st.lat_n++;
        if (lat > s_st.lat_max) s_st.lat_max = lat;
    }
}

static void do_cmd(void)
{
    const game_cmd_t c = s_cmd;
    if (c == CMD_NONE) return;
    s_cmd = CMD_NONE;
    if (c == CMD_START) game_new();
    else if (c == CMD_PAUSE) g.paused = !g.paused;
    else if (c == CMD_RESET) {
        wave_init(); erase(&g.skiff_d);
        g.st = GS_TITLE; g.score = 0U; g.paused = false;
    }
}

static u32 task_game(void *arg)
{
    (void)arg;
    g.rng = 0x1234567U ^ ca_os_tick_ms();
    g.st = GS_TITLE;
    wave_init();
    (void)ca_fb_clear(s_fb, C_BLACK);
    u32 next = ca_os_tick_ms();
    s_st.fps_t0 = next;
    while (1) {
        const u32 c0 = DWT->CYCCNT;
        do_cmd();
        if (!g.paused) game_step();
        hold_release();
        game_draw();
        hud_update();
        hud_copy();
        (void)ca_fb_present(s_fb);
        const u32 now = ca_os_tick_ms();
        stats_frame(cyc_to_us(DWT->CYCCNT - c0), now);
        next += FRAME_MS;
        if ((i32)(next - now) > 0) ca_os_sleep(next - now);
        else { s_st.late++; next = now; }
    }
    return 0U;
}

/* ============================================================================
 * Network: DHCP, ARP, net task (as ex141)
 * ============================================================================ */
static void on_link_change(bool up);
static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U,0x00U,0x00U,0x00U,0x06U,0x01U } },
    .link_cb  = on_link_change,
};

static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b   = ca_net_buf_ptr(buf_idx);
    u8           *frm = b->data + b->data_off;
    u16           l4  = b->meta.rx.l4_off;
    const ca_udp_frame_t *udp = (const ca_udp_frame_t *)(frm + l4);
    ca_dhcp_service_rx_handler(udp, (u16)(b->data_len - l4));
    ca_net_buf_free(buf_idx);
}

static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    (void)mask; (void)gw; (void)dns;
    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u", ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
    ca_arp_send_garp();
    ca_sem_post(&s_dhcp_sem);
}

static void on_dhcp_expire(void) { ca_stdout_put_n("DHCP lease EXPIRED"); }

static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U,0x00U,0x00U,0x00U,0x06U,0x01U } },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("\n\n=== APP6: Comet Guard over VNC ===");
    ca_net_proto_init(&net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    ca_eth_link_poll();
    ca_os_sleep(200);
    ca_dhcp_service_net_ready();
    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) {
        ca_eth_link_poll();
        ca_led_toggle(3U);
        ca_os_sleep(500);
    }
    return 0U;
}

static u32 task_app(void *arg)
{
    (void)arg;
    ca_sem_wait(&s_dhcp_sem);
    const ca_vnc_cfg_t cfg = {
        .fb = s_fb, .port = 0U, .name = "caOS Comet Guard",
        .security = CA_VNC_SECURITY_AUTO,
#if EN_VNC_TLS
        .cert_der = TLS_IDENTITY_CERT, .cert_len = sizeof(TLS_IDENTITY_CERT),
        .private_key = TLS_IDENTITY_KEY,
        .cred = &VNC_CRED,              /* X509Plain login: caos / caos-vnc-demo */
#endif
        .client_input = true, .on_key = on_key, .on_pointer = on_pointer,
    };
    const bool ok = ca_vnc_service_init(&cfg) && ca_vnc_service_start();
    ca_vnc_stats_t st;
    for (u32 i = 0U; i < 20U; i++) {
        ca_vnc_service_stats(&st);
        if (!ok || st.running || st.last_close != NULL) break;
        ca_os_sleep(100U);
    }
    if (ok && st.running)
        ca_stdout_printf_n("VNC listening on port %u, %s", (u32)VNC_DEFAULT_PORT,
                           st.tls ? "TLS" : "no encryption, view only");
    else
        ca_stdout_printf_n("VNC FAILED: %s", st.last_close != NULL ? st.last_close : "init");
    return 0U;
}

/* ============================================================================
 * CLI
 * ============================================================================ */
#if ENABLE_CLI
/* [n] frames a console key is held (def when no number; 1..90) */
static void console_key(hold_key_t k, const char *arg, u32 def)
{
    u32 n = (*arg >= '0' && *arg <= '9') ? (u32)ca_strtol(arg, 10) : def;
    if (n == 0U) n = 1U;
    if (n > 90U) n = 90U;
    if (s_hold[k] == 0U) local_key(true, s_hold_sym[k]);
    s_hold[k] = n;
}

/* Game keys and app6, one entry per word. All answer at once (they only set
 * a request the game loop takes), so they run inside cli: a key typed twice
 * quickly must not find the first one still "running" in a cli_cmd. */
static bool left_cmd(c_str *args)   { console_key(HK_LEFT,  (*args)->data, 5U); return true; }
static bool right_cmd(c_str *args)  { console_key(HK_RIGHT, (*args)->data, 5U); return true; }
static bool shield_cmd(c_str *args) { console_key(HK_UP,    (*args)->data, 2U); return true; }
static bool fire_cmd(c_str *args)   { console_key(HK_FIRE,  (*args)->data, 1U); return true; }

static bool credit_cmd(c_str *args)
{
    (void)args;
    if (g.st == GS_TITLE || g.st == GS_OVER) { s_cmd = CMD_START; CLI_PUT_N("APP6 credit: new game"); }
    else CLI_PUT_N("APP6 credit: a game is running");
    return true;
}

/* app6 start|pause|reset|stats */
static bool app6_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "start", 5) == 0)      { s_cmd = CMD_START; CLI_PUT_N("APP6 new game"); }
    else if (ca_strncmp(p, "pause", 5) == 0) { s_cmd = CMD_PAUSE; CLI_PUT_N("APP6 pause toggled"); }
    else if (ca_strncmp(p, "reset", 5) == 0) { s_cmd = CMD_RESET; CLI_PUT_N("APP6 back to the title"); }
    else if (ca_strncmp(p, "stats", 5) == 0) {
        ca_fb_info_t fi;
        const bool fok = ca_fb_info(s_fb, &fi);
        CLI_PRINTF_N("APP6 frames %u  fps %u  late %u  work avg %u us max %u us",
                     s_st.frames, s_st.fps, s_st.late,
                     s_st.work_n ? s_st.work_us_sum / s_st.work_n : 0U, s_st.work_us_max);
        CLI_PRINTF_N("APP6 fb buffers %u copies A->B %u  VNC %u B/s  key->frame avg %u ms max %u ms (%u keys)",
                     fok ? (u32)fi.buffers : 0U, fok ? fi.copies : 0U, s_st.vnc_bps,
                     s_st.lat_n ? s_st.lat_sum / s_st.lat_n : 0U, s_st.lat_max, s_st.lat_n);
        CLI_PRINTF_N("APP6 state %u wave %u score %u ships %u  heap free %u",
                     (u32)g.st, g.wave, g.score, g.ships, ca_memory_get_avail_memory());
        s_st.work_us_max = 0U; s_st.lat_max = 0U;
    } else {
        return false;               /* cli prints the help line */
    }
    return true;
}

static const ca_cli_cmd_t s_app6_cmds[] = {
    { "app6",   "app6      : start | pause | reset | stats   - Comet Guard over VNC", { app6_cmd,   (ca_stack_size_t)0 } },
    { "left",   "left      : [n] hold left n frames (5)",                          { left_cmd,   (ca_stack_size_t)0 } },
    { "right",  "right     : [n] hold right n frames (5)",                         { right_cmd,  (ca_stack_size_t)0 } },
    { "shield", "shield    : [n] shield n frames (2)",                             { shield_cmd, (ca_stack_size_t)0 } },
    { "up",     "up        : [n] same as shield",                                  { shield_cmd, (ca_stack_size_t)0 } },
    { "fire",   "fire      : fire",                                                { fire_cmd,   (ca_stack_size_t)0 } },
    { "btn1",   "btn1      : same as fire",                                        { fire_cmd,   (ca_stack_size_t)0 } },
    { "credit", "credit    : new game (title or game over)",                       { credit_cmd, (ca_stack_size_t)0 } },
};

static ca_cli_table_t s_app6_table = {
    "app_6", s_app6_cmds, (u16)(sizeof(s_app6_cmds) / sizeof(s_app6_cmds[0])), NULL
};
#endif

/* ============================================================================
 * GPIO ETH (NUCLEO-H753ZI, as ex139)
 * ============================================================================ */
typedef struct { u8 port; u8 pin; } eth_pin_t;
static void eth_gpio_init(void)
{
    static const eth_pin_t pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U }, { CA_GPIO_PORT_B, 13U },
    };
    for (u32 i = 0U; i < sizeof(pins) / sizeof(pins[0]); i++) {
        ca_gpio_configure(pins[i].port, pins[i].pin,
            CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
            CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, ETH_AF);
    }
}

/* ============================================================================
 * caOS hooks
 * ============================================================================ */
PRIVATE_STRING(nofb, "app6: no memory for the framebuffers");
PRIVATE_STRING(nogui, "app6: LVGL (status line) did not start");

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
        ca_cli_register_table(&s_app6_table);
    )
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    sprites_init();
    s_fb  = ca_fb_create(SCR_W, SCR_H, CA_FB_FMT_RGB565, 2U);   /* falls back to 1 */
    s_hud = ca_fb_create(SCR_W, HUD_H, CA_FB_FMT_RGB565, 1U);
    if (s_fb == CA_FB_NONE || s_hud == CA_FB_NONE) PUT_N(&priv_nofb);

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");
    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    }
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);
    ca_sem_init(&s_dhcp_sem, "DHCPBND", 0U, 1U);
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_tcp_service_start();
    ca_os_job("net",     CA_TH_LEVEL4, task_net,     NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("app",     CA_TH_LEVEL3, task_app,     NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    if (s_fb != CA_FB_NONE && s_hud != CA_FB_NONE) {
        const ca_lvgl_cfg_t lc = { .fb = s_hud, .buf_lines = HUD_H, .extra_heap = 0U,
                                   .use_dma2d = true, .cursor = false,
                                   .setup = hud_build, .setup_arg = NULL };
        if (!ca_lvgl_start(&lc)) PUT_N(&priv_nogui);
        ca_os_job("game", CA_TH_LEVEL3, task_game, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    }
    ca_os_run(true);
}

#else

PRIVATE_STRING(m6a, "=== app_6: Comet Guard over VNC ===");
PRIVATE_STRING(m6b, "DISABLED - written for the NUCLEO-H753ZI: Ethernet, EN_FB, EN_VNC_SERVICE, RAM for two 320x240 buffers");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m6a);
    PUT_N(&priv_m6b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
