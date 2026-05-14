![Example Image](deco.png)
# symsav-deco

A recursive rectangle subdivision screensaver for [SymbOS](https://www.symbos.org/) on the Amstrad CPC.

> **Alpha version — use at your own risk.** This software is in an early alpha state and may cause harm to your system. If you choose to try it, you do so entirely at your own risk.

> **Requires Mode 1** — this screensaver only works in 320×200 Mode 1 (4 colours). Running it in any other screen mode will produce incorrect output.

Inspired by Jamie Zawinski's [deco](https://www.jwz.org/xscreensaver/) from the xscreensaver suite (concept originally by Michael D. Bayne).

---

## Building

```bash
./build.sh
```

Requires the SCC compiler (set `SCC=` env var if not at `../scc/bin/cc`) and Python 3.

Build steps:

1. SCC compiles `deco.c` → `deco.sav`
2. `add_preview.py` patches the preview thumbnail into the binary at file offset 256

Output: `deco.sav`

---

## Installing

1. Copy `deco.sav` into your `C:\SYMBOS\` directory.
2. Open **Display Properties** and go to the **Screen Saver** tab.
3. Click **Browse** and select `deco.sav`.
4. Click **Setup** to configure the effect:
   - **Depth**: Shallow / Normal / Deep — maximum recursion depth (4 / 6 / 8 levels)
   - **Split**: Random (50/50) or Golden (62/38 ratio, always splits the longer axis)
   - **Speed**: Slow / Normal / Fast — ticks between full redraws (300 / 150 / 60)

---

## Effect

- The full 320×200 screen is recursively divided into axis-aligned rectangles
- Each leaf rectangle is filled with one of three inks (bright, white, dim) chosen at random; the black background shows through as a 4-pixel border between panels
- After the configured pause the screen clears and a new random subdivision is drawn
- Uses SymbOS default Mode 1 palette: ink1 (black) as background/border, inks 0/2/3 as fill colours

The subdivision loop is implemented iteratively with an explicit work-stack in the data segment (`DecoItem deco_stack[32]`) to avoid relying on deep SCC call-stack recursion.

---

## Screensaver protocol

Standard SymbOS screensaver messages:

| Message | Action |
|---------|--------|
| `MSC_SAV_INIT` (1) | Load saved config from manager |
| `MSC_SAV_START` (2) | Start fullscreen animation |
| `MSC_SAV_CONFIG` (3) | Open config dialog |
| `MSR_SAV_CONFIG` (4) | Send updated config back |

Config is 7 bytes: magic `"DECO"` + depth byte + split byte + speed byte.

---

## Animation

Fullscreen rendering follows the same approach as [symsav-xroach](https://github.com/salvogendut/symsav-xroach) and [symsav-xmatrix](https://github.com/salvogendut/symsav-xmatrix):

1. Open a fullscreen `WIN_NOTTASKBAR | WIN_NOTMOVEABLE` window
2. `DSK_SRV_DSKSTP` to freeze the desktop
3. Clear all 8 CPC character planes via `Bank_Copy` to VRAM (bank 0, `0xC000`)
4. Per frame: decrement pause timer; when it expires, clear and redraw a new subdivision
5. Exit on any key or mouse movement: resume desktop, close window, `Screen_Redraw()`

Rectangle fill formula for a byte-aligned rect at pixel column `x`, row `y`, byte-width `bw`:

```
addr = 0xC000 + (y/8)*80 + (y%8)*0x800 + x/4
Bank_Copy(addr, fill_buf, bw)   // repeated for each scanline
```
