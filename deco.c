// deco.c — Deco screensaver for SymbOS (CPC and MSX)
// Inspired by xscreensaver's "deco" (Jamie Zawinski, 1997) — recursive
// rectangle subdivision filling the screen with coloured panels.
//
// CPC: renders to Mode 1 VRAM (320x200, 4 inks) via Bank_Copy.
// MSX: renders to Screen 7 VRAM (512x212, 16 colors) via VDP ports.
// SymbOS C port by Salvatore Bognanni

#include <symbos.h>
#include <symbos/msgid.h>
#include <symbos/keys.h>
#include <stdlib.h>
#include <string.h>

#define MSC_SAV_INIT   1
#define MSC_SAV_START  2
#define MSC_SAV_CONFIG 3
#define MSR_SAV_CONFIG 4

// CPC Mode 1 — 320x200, 4 inks, 4px per byte
#define SCREEN_W_CPC  320
#define SCREEN_H_CPC  200

// MSX Screen 7 — 512x212, 16 colors, 2px per byte (4bpp nibble)
#define SCREEN_W_MSX  512
#define SCREEN_H_MSX  212

// Border: 4px gap between panels (background ink shows through).
// Must be a multiple of 4 so x stays 4-pixel aligned on CPC (1 Mode-1 byte)
// and 2-pixel aligned on MSX (1 Screen-7 byte).
#define BORDER    4

// Minimum panel size before recursion stops regardless of depth.
#define MIN_W     20
#define MIN_H     20

// ---------------------------------------------------------------------------
// CPC Mode-1 VRAM encoding
//
// Byte layout for 4 consecutive pixels p0..p3 (ink 0-3):
//   bit7=p0_lo  bit6=p1_lo  bit5=p2_lo  bit4=p3_lo
//   bit3=p0_hi  bit2=p1_hi  bit1=p2_hi  bit0=p3_hi
//
// SymbOS default Mode-1 palette:
//   ink0 = white  -> 0x00
//   ink1 = black  -> 0xF0  (background / border)
//   ink2 = dim    -> 0x0F
//   ink3 = bright -> 0xFF
// ---------------------------------------------------------------------------

static const unsigned char ink_byte[4] = { 0x00, 0xF0, 0x0F, 0xFF };

// CPC fill inks: indices into ink_byte[] used for leaf panels.
static const unsigned char fill_inks[3] = { 3, 0, 2 };   // bright, white, dim

// ---------------------------------------------------------------------------
// MSX Screen 7 color encoding
//
// Each byte holds 2 pixels as 4-bit nibbles: high = left pixel, low = right.
// Nibble value = SymbOS color index (1=black background, 8=white, ...).
// A solid fill byte = (color << 4) | color.
//
// Fill colors — vibrant set from the 16-color MSX palette:
//   0x8 = COLOR_WHITE,  0x9 = COLOR_GREEN,   0xA = COLOR_LGREEN,
//   0xB = COLOR_LRED,   0xC = COLOR_YELLOW,  0xD = COLOR_GRAY,
//   0xE = COLOR_LCYAN,  0xF = COLOR_LBLUE
// ---------------------------------------------------------------------------

#define MSX_BG      0x1   // black (border / background)
#define MSX_NCOLORS 8

static const unsigned char msx_fill_inks[8] = {
    0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF
};

// VDP fill routine (deco_msx.s): fills len bytes of MSX VRAM at vram_addr.
extern void vdp_fill(unsigned int vram_addr, unsigned char fill_byte,
                     unsigned short len);

// ---------------------------------------------------------------------------
// Data-segment buffers
// ---------------------------------------------------------------------------

// Full-screen clear plane: 25 char rows * 80 bytes = 2000 bytes at ink1 (0xF0).
_data unsigned char zero_plane[2000];

// Row fill buffer: up to 80 bytes for one horizontal fill scanline.
_data unsigned char fill_buf[80];

// Config: [0..3]="DECO" magic, [4]=max_depth (1-3), [5]=split (0=random 1=golden), [6]=speed (1-3)
_data char cfgdat[64];
_data char init_tmp[64];

// Iterative deco work stack — five separate int arrays instead of a struct
// array so that indexing uses stride=2 (a single shift), which SCC handles
// reliably.  Stride=10 (struct size) requires a multiply that SCC gets wrong.
#define DECO_STACK_MAX 32
_data int stk_x[DECO_STACK_MAX];
_data int stk_y[DECO_STACK_MAX];
_data int stk_w[DECO_STACK_MAX];
_data int stk_h[DECO_STACK_MAX];
_data int stk_d[DECO_STACK_MAX];

// Leaf panel staging buffer: deco_plan() fills these; deco_render() draws them.
// VRAM is only written after we confirm the plan has >= 2 panels, so a
// degenerate single-colour frame is never visible.
#define MAX_LEAVES 48
_data int           lf_x[MAX_LEAVES];
_data int           lf_y[MAX_LEAVES];
_data int           lf_w[MAX_LEAVES];
_data int           lf_h[MAX_LEAVES];
_data unsigned char lf_ink[MAX_LEAVES];

// ---------------------------------------------------------------------------
// Platform state
// ---------------------------------------------------------------------------

// Set at animation start via Sys_Type(); 0=CPC, 1=MSX.
_transfer char          is_msx = 0;

// Runtime screen dimensions (set from SCREEN_W/H_CPC or _MSX at startup).
_transfer int           scr_w;
_transfer int           scr_h;

// Number of fill inks available on the current platform (3 for CPC, 8 for MSX).
_transfer unsigned char deco_ninks;

// ---------------------------------------------------------------------------
// Animation state
// ---------------------------------------------------------------------------

_transfer unsigned char deco_max_depth;  // recursion depth limit
_transfer unsigned char deco_split;      // 0=random, 1=golden
_transfer int           anim_timer;      // ticks remaining until next redraw
_transfer int           anim_pause;      // configured pause length in ticks
_transfer unsigned char anim_stage;      // 0=showing panel, 1=trigger redraw

// ---------------------------------------------------------------------------
// Screen clear
// ---------------------------------------------------------------------------

static void vram_clear(void)
{
    unsigned char k;
    if (is_msx) {
        // Screen 7: 212 rows x 256 bytes/row = 54272 bytes; ink1+ink1 = 0x11
        vdp_fill(0u, 0x11u, 54272u);
    } else {
        for (k = 0; k < 8; k++) {
            Bank_Copy(0,
                (char *)(0xC000u + (unsigned short)k * 0x0800u),
                _symbank, (char *)zero_plane, 2000u);
        }
    }
}

// ---------------------------------------------------------------------------
// Fill an aligned rectangle with a solid ink.
// x must be a multiple of 4; w must be a multiple of 4.
// ---------------------------------------------------------------------------

static void vram_fill_rect(int x, int y, int w, int h, unsigned char ink)
{
    unsigned short addr;
    unsigned int   msx_addr;
    unsigned char  fill_byte;
    int row, bx, bw;

    if (is_msx) {
        // Screen 7: 2px per byte; each row is 256 bytes; VRAM is linear.
        bx = x >> 1;
        bw = w >> 1;
        if (bw <= 0 || h <= 0) return;
        fill_byte = (unsigned char)((ink << 4) | ink);
        for (row = y; row < y + h; row++) {
            msx_addr = (unsigned int)row * 256u + (unsigned int)bx;
            vdp_fill(msx_addr, fill_byte, (unsigned short)bw);
        }
    } else {
        // CPC Mode 1: 4px per byte; VRAM is interleaved across 8 planes.
        bx = x >> 2;
        bw = w >> 2;
        if (bw <= 0 || h <= 0) return;
        memset(fill_buf, ink_byte[ink & 3], (unsigned short)bw);
        for (row = y; row < y + h; row++) {
            addr = 0xC000u
                 + (unsigned short)(row >> 3) * 80u
                 + (unsigned short)(row &  7) * 0x0800u
                 + (unsigned short)bx;
            Bank_Copy(0, (char *)addr, _symbank, (char *)fill_buf, (unsigned short)bw);
        }
    }
}

// ---------------------------------------------------------------------------
// Iterative deco — processes the work stack until empty.
// All splits keep x 4-pixel aligned so vram_fill_rect can address full bytes.
// ---------------------------------------------------------------------------

// Plan a deco frame: run the subdivision, storing each leaf panel in the
// lf_* arrays instead of touching VRAM.  Returns the number of leaves found.
static int deco_plan(void)
{
    int x, y, w, h, depth, wnew, hnew, ink_idx, do_hsplit, top, leaves;

    stk_x[0] = 0;  stk_y[0] = 0;
    stk_w[0] = scr_w;  stk_h[0] = scr_h;  stk_d[0] = 0;
    top    = 1;
    leaves = 0;

    while (top > 0) {

        top--;
        x     = stk_x[top];
        y     = stk_y[top];
        w     = stk_w[top];
        h     = stk_h[top];
        depth = stk_d[top];

        // Leaf condition: too small, or probability threshold reached.
        // Guard deco_max_depth > 0 prevents division by zero.
        if (w < MIN_W || h < MIN_H ||
            (deco_max_depth > 0 && (rand() % (int)deco_max_depth) < depth)) {

            ink_idx = rand() % (int)deco_ninks;
            if (leaves < MAX_LEAVES) {
                lf_x[leaves]   = x + BORDER;
                lf_y[leaves]   = y + BORDER;
                lf_w[leaves]   = (w - 2 * BORDER) & ~3;
                lf_h[leaves]   = h - 2 * BORDER;
                lf_ink[leaves] = is_msx ? msx_fill_inks[ink_idx]
                                        : fill_inks[ink_idx];
            }
            leaves++;

        } else {

            // Decide split axis.
            if (deco_split == 1) {
                do_hsplit = (w > h) ? 1 : 0;
            } else {
                do_hsplit = rand() & 1;
            }

            if (do_hsplit) {
                // Side-by-side split
                if (deco_split == 1)
                    wnew = (rand() & 1) ? (w * 38 / 100) : (w * 62 / 100);
                else
                    wnew = w >> 1;
                wnew &= ~3;
                if (wnew < BORDER + 4)       wnew = BORDER + 4;
                if (wnew > w - BORDER - 4)   wnew = w - BORDER - 4;
                wnew &= ~3;

                if (top + 2 <= DECO_STACK_MAX) {
                    stk_x[top] = x;        stk_y[top] = y;
                    stk_w[top] = wnew;     stk_h[top] = h;
                    stk_d[top] = depth + 1;
                    top++;
                    stk_x[top] = x + wnew; stk_y[top] = y;
                    stk_w[top] = w - wnew;  stk_h[top] = h;
                    stk_d[top] = depth + 1;
                    top++;
                }

            } else {
                // Top-to-bottom split
                if (deco_split == 1)
                    hnew = (rand() & 1) ? (h * 38 / 100) : (h * 62 / 100);
                else
                    hnew = h >> 1;
                if (hnew < BORDER + 1)       hnew = BORDER + 1;
                if (hnew > h - BORDER - 1)   hnew = h - BORDER - 1;

                if (top + 2 <= DECO_STACK_MAX) {
                    stk_x[top] = x;  stk_y[top] = y;
                    stk_w[top] = w;  stk_h[top] = hnew;
                    stk_d[top] = depth + 1;
                    top++;
                    stk_x[top] = x;       stk_y[top] = y + hnew;
                    stk_w[top] = w;       stk_h[top] = h - hnew;
                    stk_d[top] = depth + 1;
                    top++;
                }
            }
        }
    }
    return leaves;
}

// Render the leaf panels stored by the most recent deco_plan() call.
static void deco_render(int n)
{
    int i;
    if (n > MAX_LEAVES) n = MAX_LEAVES;
    for (i = 0; i < n; i++)
        vram_fill_rect(lf_x[i], lf_y[i], lf_w[i], lf_h[i], lf_ink[i]);
}

// Plan up to 8 times until we get >= 2 panels, then clear + render.
// VRAM is never written for a degenerate single-colour frame.
static void deco_draw_checked(void)
{
    unsigned char tries;
    int n;
    for (tries = 0; tries < 8; tries++) {
        n = deco_plan();
        if (n >= 3) {
            vram_clear();
            deco_render(n);
            return;
        }
    }
    // Fallback: render whatever the last plan produced.
    vram_clear();
    deco_render(n);
}

// ---------------------------------------------------------------------------
// Animation tick
// ---------------------------------------------------------------------------

static void anim_tick(void)
{
    if (anim_stage == 0) {
        if (--anim_timer <= 0)
            anim_stage = 1;
    } else {
        deco_draw_checked();
        anim_timer = anim_pause;
        anim_stage = 0;
    }
}

// ---------------------------------------------------------------------------
// Key scan (hardware poll — works while desktop is stopped)
// ---------------------------------------------------------------------------

static unsigned char any_key_down(void)
{
    unsigned char sc;
    for (sc = 0; sc < 80; sc++)
        if (Key_Down(sc)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Config dialog
// ---------------------------------------------------------------------------

_transfer char        tmp_depth  = 2;
_transfer char        tmp_split  = 0;
_transfer char        tmp_speed  = 2;
_transfer char        cfg_prz    = 0;
_transfer signed char cfgwin_id  = -1;

_transfer char rg_depth[4] = { -1, -1, -1, -1 };
_transfer char rg_split[4] = { -1, -1, -1, -1 };
_transfer char rg_speed[4] = { -1, -1, -1, -1 };

_transfer Ctrl_TFrame cfg_tf    = { "Settings", (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_d = { "Depth:",   (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_p = { "Split:",   (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_s = { "Speed:",   (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };

_transfer Ctrl_Radio cfg_rad_d1 = { &tmp_depth, "Shallow", (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_depth };
_transfer Ctrl_Radio cfg_rad_d2 = { &tmp_depth, "Normal",  (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_depth };
_transfer Ctrl_Radio cfg_rad_d3 = { &tmp_depth, "Deep",    (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_depth };
_transfer Ctrl_Radio cfg_rad_p1 = { &tmp_split, "Random",  (COLOR_BLACK<<2)|COLOR_ORANGE, 0, rg_split };
_transfer Ctrl_Radio cfg_rad_p2 = { &tmp_split, "Golden",  (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_split };
_transfer Ctrl_Radio cfg_rad_s1 = { &tmp_speed, "Slow",    (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_speed };
_transfer Ctrl_Radio cfg_rad_s2 = { &tmp_speed, "Normal",  (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_speed };
_transfer Ctrl_Radio cfg_rad_s3 = { &tmp_speed, "Fast",    (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_speed };

// Layout: 220 x 74 px.  Frame "Settings" rows 0..50.  Buttons at y=58.
_transfer Ctrl ccc0  = { 0,  C_AREA,   -1, COLOR_ORANGE,                     0,  0, 220, 74, 0 };
_transfer Ctrl ccc1  = { 0,  C_TFRAME, -1, (unsigned short)&cfg_tf,          2,  1, 216, 50, 0 };
_transfer Ctrl ccc2  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_d,       8, 10,  38,  8, 0 };
_transfer Ctrl ccc3  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d1,     50, 10,  52,  8, 0 };
_transfer Ctrl ccc4  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d2,    104, 10,  44,  8, 0 };
_transfer Ctrl ccc5  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_d3,    150, 10,  34,  8, 0 };
_transfer Ctrl ccc6  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_p,       8, 22,  38,  8, 0 };
_transfer Ctrl ccc7  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_p1,     50, 22,  52,  8, 0 };
_transfer Ctrl ccc8  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_p2,    104, 22,  44,  8, 0 };
_transfer Ctrl ccc9  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_s,       8, 34,  38,  8, 0 };
_transfer Ctrl ccc10 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s1,     50, 34,  30,  8, 0 };
_transfer Ctrl ccc11 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s2,     82, 34,  44,  8, 0 };
_transfer Ctrl ccc12 = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s3,    128, 34,  30,  8, 0 };
_transfer Ctrl ccc13 = { 10, C_BUTTON, -1, (unsigned short)"OK",             60, 58,  32, 12, 0 };
_transfer Ctrl ccc14 = { 11, C_BUTTON, -1, (unsigned short)"Cancel",        100, 58,  52, 12, 0 };

_transfer Ctrl_Group cfgcg;
_transfer Window     cfgwin;
_transfer char       cfg_title[5] = { 'D', 'e', 'c', 'o', 0 };

_transfer Ctrl       anim_ctrl[1];
_transfer Ctrl_Group anim_cg;
_transfer Window     anim_win;
_transfer char       empty_str[1];

// ---------------------------------------------------------------------------

static void cfg_open(void)
{
    if (cfgwin_id >= 0) return;

    tmp_depth = cfgdat[4];
    tmp_split = cfgdat[5];
    tmp_speed = cfgdat[6];

    rg_depth[0] = rg_depth[1] = rg_depth[2] = rg_depth[3] = -1;
    rg_split[0] = rg_split[1] = rg_split[2] = rg_split[3] = -1;
    rg_speed[0] = rg_speed[1] = rg_speed[2] = rg_speed[3] = -1;

    memset(&cfgcg, 0, sizeof(cfgcg));
    cfgcg.controls = 15;
    cfgcg.pid      = _sympid;
    cfgcg.first    = &ccc0;

    memset(&cfgwin, 0, sizeof(cfgwin));
    cfgwin.state    = WIN_NORMAL;
    cfgwin.flags    = WIN_TITLE | WIN_CENTERED | WIN_NOTTASKBAR;
    cfgwin.pid      = _sympid;
    cfgwin.w        = 220;
    cfgwin.h        = 74;
    cfgwin.wfull    = 220;
    cfgwin.hfull    = 74;
    cfgwin.wmin     = 220;
    cfgwin.hmin     = 74;
    cfgwin.wmax     = 220;
    cfgwin.hmax     = 74;
    cfgwin.title    = cfg_title;
    cfgwin.controls = &cfgcg;

    cfgwin_id = Win_Open(_symbank, &cfgwin);
}

static void cfg_close(void)
{
    if (cfgwin_id < 0) return;
    Win_Close((unsigned char)cfgwin_id);
    cfgwin_id = -1;
}

static void cfg_ok(void)
{
    cfgdat[4] = tmp_depth;
    cfgdat[5] = tmp_split;
    cfgdat[6] = tmp_speed;
    cfg_close();
    if (cfg_prz) {
        _symmsg[0] = MSR_SAV_CONFIG;
        _symmsg[1] = _symbank;
        _symmsg[2] = (char)((unsigned short)cfgdat & 0xFF);
        _symmsg[3] = (char)((unsigned short)cfgdat >> 8);
        while (!Msg_Send(_sympid, cfg_prz, _symmsg));
        cfg_prz = 0;
    }
}

static void cfg_cancel(void)
{
    cfg_close();
    cfg_prz = 0;
}

// ---------------------------------------------------------------------------
// Desktop stop / resume
// ---------------------------------------------------------------------------

static void desktop_stop(unsigned char wid)
{
    _symmsg[0] = MSC_DSK_DSKSRV;
    _symmsg[1] = DSK_SRV_DSKSTP;
    _symmsg[2] = 0xFF;
    _symmsg[3] = wid;
    while (Msg_Send(_sympid, 2, _symmsg) == 0);
    Msg_Wait(_sympid, 2, _symmsg, MSR_DSK_DSKSRV);
}

static void desktop_cont(void)
{
    _symmsg[0] = MSC_DSK_DSKSRV;
    _symmsg[1] = DSK_SRV_DSKCNT;
    while (Msg_Send(_sympid, 2, _symmsg) == 0);
    Idle();
}

// ---------------------------------------------------------------------------
// Animation entry point
// ---------------------------------------------------------------------------

void start_animation(void)
{
    signed char    wid;
    unsigned char  speed, depth, split, b;
    unsigned short mx0, my0;
    unsigned short resp;

    depth = (unsigned char)cfgdat[4];
    split = (unsigned char)cfgdat[5];
    speed = (unsigned char)cfgdat[6];

    if (depth < 1 || depth > 3) depth = 2;
    if (split > 1)               split = 0;
    if (speed < 1 || speed > 3) speed = 2;

    // Detect platform and set runtime screen geometry.
    is_msx = ((Sys_Type() & TYPE_MSX) != 0) ? 1 : 0;
    if (is_msx) {
        scr_w      = SCREEN_W_MSX;
        scr_h      = SCREEN_H_MSX;
        deco_ninks = MSX_NCOLORS;
    } else {
        scr_w      = SCREEN_W_CPC;
        scr_h      = SCREEN_H_CPC;
        deco_ninks = 3;
    }

    // Original xscreensaver deco uses max_depth=12 as default.
    // For 320x200 with min_size=20, log2(320/20)=4 halvings reach min width,
    // so values well above 4 are needed for meaningful subdivision.
    // Same values serve MSX well (512x212 is proportionally similar).
    deco_max_depth = (depth == 1) ? 8 : (depth == 3) ? 16 : 12;
    deco_split     = split;

    // Pause ticks between redraws: slow=300, normal=150, fast=60.
    anim_pause = (speed == 1) ? 300 : (speed == 3) ? 60 : 150;

    srand((unsigned int)Sys_Counter());

    memset(zero_plane, 0xF0, sizeof(zero_plane));

    empty_str[0] = 0;

    anim_ctrl[0].value  = 0;
    anim_ctrl[0].type   = C_AREA;
    anim_ctrl[0].bank   = -1;
    anim_ctrl[0].param  = AREA_16COLOR | COLOR_BLACK;
    anim_ctrl[0].x      = 0;
    anim_ctrl[0].y      = 0;
    anim_ctrl[0].w      = scr_w;
    anim_ctrl[0].h      = scr_h;
    anim_ctrl[0].unused = 0;

    memset(&anim_cg, 0, sizeof(anim_cg));
    anim_cg.controls = 1;
    anim_cg.pid      = _sympid;
    anim_cg.first    = &anim_ctrl[0];

    memset(&anim_win, 0, sizeof(anim_win));
    anim_win.state    = WIN_NORMAL;
    anim_win.flags    = WIN_NOTTASKBAR | WIN_NOTMOVEABLE;
    anim_win.pid      = _sympid;
    anim_win.w        = scr_w;
    anim_win.h        = scr_h;
    anim_win.wfull    = scr_w;
    anim_win.hfull    = scr_h;
    anim_win.wmin     = 32;
    anim_win.hmin     = 24;
    anim_win.wmax     = scr_w;
    anim_win.hmax     = scr_h;
    anim_win.title    = empty_str;
    anim_win.status   = empty_str;
    anim_win.controls = &anim_cg;

    wid = Win_Open(_symbank, &anim_win);
    if (wid < 0) return;

    desktop_stop((unsigned char)wid);
    vram_clear();

    // CPC only: one Idle() to flush any deferred system VRAM writes, then
    // re-clear the bottom char row (y=192-199) which the taskbar clock may
    // have touched.
    if (!is_msx) {
        Idle();
        for (b = 0; b < 8; b++)
            Bank_Copy(0, (char *)(0xC000u + (unsigned short)b * 0x0800u + 1920u),
                      _symbank, (char *)zero_plane, 80u);
    }

    // First draw.
    deco_draw_checked();
    anim_timer = anim_pause;
    anim_stage = 0;

    mx0 = Mouse_X();
    my0 = Mouse_Y();

    while (1) {

        if (Mouse_X()    != mx0 ||
            Mouse_Y()    != my0 ||
            Mouse_Buttons()     ||
            any_key_down()) {

            desktop_cont();
            Idle();
            Win_Close((unsigned char)wid);
            Screen_Redraw();
            return;
        }

        resp = Msg_Receive(_sympid, -1, _symmsg);
        if (resp & 1) {
            if (_symmsg[0] == 0) {
                desktop_cont();
                Win_Close((unsigned char)wid);
                exit(0);
            }
        }

        anim_tick();

        Idle();
        if (!is_msx) {
            for (b = 0; b < 8; b++)
                Bank_Copy(0, (char *)(0xC000u + (unsigned short)b * 0x0800u + 1920u),
                          _symbank, (char *)zero_plane, 80u);
        }
    }
}

// ---------------------------------------------------------------------------
// Main — screensaver protocol (identical structure to xmatrix / mountain)
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    unsigned short resp;
    unsigned char  got_msg, sender, b;

    // _data vars are not statically initialised by SCC — set defaults here.
    cfgdat[0] = 'D'; cfgdat[1] = 'E'; cfgdat[2] = 'C'; cfgdat[3] = 'O';
    cfgdat[4] = 2;   /* depth: normal */
    cfgdat[5] = 0;   /* split: random */
    cfgdat[6] = 2;   /* speed: normal */

    got_msg = 0;
    sender  = 0;

    for (b = 0; b < 10; b++) {
        Idle();
        resp = Msg_Receive(_sympid, -1, _symmsg);
        if (resp & 0x01) {
            got_msg = 1;
            sender  = (unsigned char)(resp >> 8);
            break;
        }
    }

    if (!got_msg) {
        start_animation();
        exit(0);
    }

    while (1) {

        switch (_symmsg[0]) {

        case 0:
            exit(0);

        case MSC_SAV_INIT:
            Bank_Copy(
                _symbank, init_tmp,
                (unsigned char)_symmsg[1],
                (char *)((unsigned short)((unsigned char)_symmsg[3] << 8)
                         | (unsigned char)_symmsg[2]),
                64u);
            if (init_tmp[0] == 'D' && init_tmp[1] == 'E' &&
                init_tmp[2] == 'C' && init_tmp[3] == 'O') {
                memcpy(cfgdat, init_tmp, 64);
            }
            break;

        case MSC_SAV_START:
            start_animation();
            break;

        case MSC_SAV_CONFIG:
            cfg_prz = sender;
            cfg_open();
            break;

        default:
            if ((unsigned char)_symmsg[0] == MSR_DSK_WCLICK &&
                cfgwin_id >= 0 &&
                (unsigned char)_symmsg[1] == (unsigned char)cfgwin_id) {

                if ((unsigned char)_symmsg[2] == DSK_ACT_CLOSE) {
                    cfg_cancel();
                } else if ((unsigned char)_symmsg[2] == DSK_ACT_CONTENT) {
                    if ((unsigned char)_symmsg[8] == 10)
                        cfg_ok();
                    else if ((unsigned char)_symmsg[8] == 11)
                        cfg_cancel();
                }
            }
            break;
        }

        do {
            resp = Msg_Sleep(_sympid, -1, _symmsg);
        } while (!(resp & 0x01));

        sender = (unsigned char)(resp >> 8);
    }
}
