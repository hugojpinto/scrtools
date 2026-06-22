# scrtools

Command line tool for manipulating ZX Spectrum `SCREEN$` (`.scr`) files —
standard resolution **and** the Timex SCLD extended modes (hi-colour and
hi-res).

It is a single, dependency-free C translation unit; PNG decoding uses the
vendored, public-domain `stb_image.h`, and PNG/GIF encoding is hand-rolled
(no zlib).

## Usage

```
scrtools command [options] FileIn [FileOut]

Commands:
    png2scr          Convert a PNG to a .scr (see --mode / --ulaplus).
    scr2png          Render a .scr to a PNG. The screen mode (standard,
                     hi-colour, hi-res, ULAplus) is detected from the size.
    scr2gif          Render a .scr to a GIF, animating the FLASH
                     attribute as a 2-frame loop (static if no FLASH).
    info             Show a summary of a .scr (mode, size, colours/palette).

Options:
    -h, --help       Display this help and exit.
    --mode <m>       Target mode for png2scr (default std):
                       std        256x192, 8x8 attrs  -> 6912 bytes
                       hicolour   256x192, 8x1 attrs  -> 12288 bytes
                       hires      512x192 mono+colour -> 12289 bytes
    --ulaplus        png2scr: emit a ULAplus 64-colour screen (+64 palette
                     bytes -> 6976 / 12352). std and hicolour only.
    --level <n>      Normal (non-bright) colour level for rendering,
                     0-255 (default 215 = 0xD7). Bright stays 255.
    --palette <p>    Named level preset: standard (215, default),
                     classic (205), emulator (192).
    --dither         png2scr: Floyd-Steinberg dither the bitmap between
                     each cell's two colours (helps shaded images).
    --scale <n>      Upscale scr2png / scr2gif output by factor n.
    --delay <cs>     FLASH frame delay for scr2gif, centiseconds
                     (default 32 = the hardware ~1.56 Hz rate).

Examples:
    scrtools png2scr factory.png factory.scr
    scrtools png2scr --mode hires logo512.png logo.scr
    scrtools png2scr --mode hicolour pic.png pic.scr
    scrtools png2scr --ulaplus photo.png photo.scr
    scrtools png2scr --ulaplus --dither photo.png photo.scr
    scrtools scr2png demo.scr demo.png
    scrtools scr2png --palette emulator demo.scr demo.png
    scrtools scr2gif --scale 2 demo.scr demo.gif
    scrtools info demo.scr
```

## Screen modes

The mode is determined by **file size** — there is no header (the de-facto
convention used by ZX-Paintbrush and the wider toolchain):

| Size  | Mode             | Layout |
|-------|------------------|--------|
| 6912  | Standard         | 6144 bitmap + 768 attrs (32×24 cells of 8×8) |
| 6976  | Standard + ULAplus | 6912 + 64 palette registers |
| 12288 | Timex hi-colour  | 6144 bitmap + 6144 attrs (8×1: one attr per scanline) |
| 12352 | hi-colour + ULAplus | 12288 + 64 palette registers |
| 12289 | Timex hi-res     | two 6144 banks (512×192 mono) + 1 colour byte |
| 12353 | hi-res + ULAplus | 12289 + 64 palette registers |

### Standard

256×192, one bit per pixel. The bitmap uses the Spectrum's interleaved address
scheme (the screen is three 64-line thirds; within a third the eight character
rows are interleaved by scanline):

```
offset = (x>>3) + (y&7)*256 + ((y>>3)&7)*32 + (y>>6)*2048
```

Each attribute byte is `FLASH(1) BRIGHT(1) PAPER(3) INK(3)`. Set pixels take the
cell's **INK** colour, clear pixels its **PAPER**. The three colour bits are
`GRB`: bit 2 = green, bit 1 = red, bit 0 = blue. `BRIGHT` raises all three
channels to full intensity. Two-colours-per-cell is the source of the famous
Spectrum "attribute clash".

### Timex hi-colour (12288 bytes)

Still 256×192, but attributes are **8×1**: one attribute byte per 8-pixel-wide,
single-scanline strip — so colour can change every row. The file is two 6144
blocks (bitmap, then attributes), and the attribute block uses the *same*
interleaved layout as the bitmap, so the attribute for pixel (x,y) sits at the
identical offset in block 2 as the pixel byte in block 1.

### Timex hi-res (12289 bytes)

512×192 monochrome. Two 6144 banks: **even** pixel columns come from block 1,
**odd** columns from block 2 (each bank a normal interleaved 256×192 bitmap).
There is a single global colour for the whole screen, stored in a trailing byte
(the raw port-`0xFF` value): bits 3-5 are the **INK** colour, **PAPER = 7 − INK**,
both `BRIGHT`. The eight possible pairs are high-contrast complements (e.g.
`0x36` → yellow ink on blue paper).

### ULAplus (64-colour palette)

Any of the three modes can carry a **ULAplus** palette: 64 registers appended to
the end of the file (so 6976 / 12352 / 12353 bytes). Each register is `GGGRRRBB`
(3-bit green, 3-bit red, 2-bit blue → 256 possible colours). When a palette is
present the attribute byte's top two bits stop meaning FLASH/BRIGHT and instead
select one of **four 16-entry CLUT groups** (8 ink + 8 paper):

```
ink_index   = group*16 + INK          (group = FLASH*2 + BRIGHT)
paper_index = group*16 + 8 + PAPER
```

A cell still shows only two colours, but each can be any of the 64 — provided
both come from the same group. There is no hardware FLASH in palette mode (the
bit is repurposed), so `scr2gif` renders ULAplus screens as a single frame.
`scr2png`/`scr2gif`/`info` apply the embedded palette automatically (the
`--level`/`--palette` options don't affect ULAplus output). Hi-res + ULAplus
(12353) is read and preserved, but its palette mapping is under-specified in the
wild, so hi-res is rendered with its fixed colour pair.

## The palette

The Spectrum is an analogue machine; "normal" brightness is ≈85% of the bright
voltage, which different tools digitise to different RGB levels. scrtools
defaults to the modern de-facto standard and lets you override it:

| `--palette` | Level | Hex  | Used by |
|-------------|-------|------|---------|
| `standard`  | 215   | 0xD7 | Wikipedia / retrotechlab (default) |
| `classic`   | 205   | 0xCD | older conversion tools (and `taput`) |
| `emulator`  | 192   | 0xC0 | Fuse / img2spec (≈75%) |

Bright lit channels are always `0xFF`; black is `0x000000` in both intensities
(hence 15 distinct colours, not 16). The `.scr` itself stores no RGB, so the
level only affects rendering — use `--level 191` to reproduce a PNG authored
with the `0xBF` convention, for example.

## Conversion notes

* **png2scr** classifies every source pixel into one of the 8 Spectrum colours
  (tolerant of the exact normal level used), then resolves ink/paper/bright per
  cell (8×8 for std, 8×1 for hi-colour) or a single global pair (hi-res). A
  faithful render converts losslessly; a clashing cell snaps to its two dominant
  colours rather than being rejected. `--dither` adds Floyd-Steinberg error
  diffusion when choosing ink-vs-paper per pixel, trading hard edges for smooth
  shading — most useful on gradients and photos.
* The rendered image is the canonical truth: `png → scr → png` is
  **pixel-identical** (at a matching `--level`), and `png2scr` output is a stable
  **fixed point**. On real Timex hi-res samples, `scr → png → scr` is even
  byte-identical. Note `scr → png → scr` is not byte-identical in general for
  attribute modes (the render can't recover an ink/paper swap, nor a colour a
  cell never paints).
* **scr2gif** animates the `FLASH` attribute as a looping 2-frame GIF (ink/paper
  swap every 320 ms, the hardware rate). GIF's indexed colour is a perfect fit
  for the 16-entry Spectrum palette (64 entries for ULAplus). Screens with no
  FLASH cells — including all ULAplus screens — produce a single static frame.
* **png2scr --ulaplus** is a constrained quantiser. It picks each cell's two
  colours by **2-means** in RGB (better than most-frequent for shaded cells),
  snaps to the GRB-332 gamut, then fits all cells into four 16-entry CLUT groups:
  a single-group lossless fast path, falling back to **k-means** group assignment
  with **median-cut** building each group's 8 ink / 8 paper slots. Images that
  already fit ULAplus constraints round-trip losslessly; richer images quantise
  to ≤64 colours, and `--dither` smooths the result.

## Roadmap

* Hi-res + ULAplus colour mapping, if a de-facto standard emerges.

## Building

```
make            # native build (auto-detects macOS / Linux / Windows)
make mac        # macOS universal binary (arm64 + x86_64)
make linux      # Linux x86_64, static (musl)
make windows    # Windows x86_64, static (mingw)
make release    # all three
make test       # build, then run the regression suite
```

Cross-compiling from macOS needs the toolchains:

```
brew install FiloSottile/musl-cross/musl-cross   # Linux target
brew install mingw-w64                            # Windows target
```
