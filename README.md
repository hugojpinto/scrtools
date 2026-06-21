# scr-tools

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
    png2scr          Convert a PNG to a .scr (see --mode).
    scr2png          Render a .scr to a PNG. The screen mode (standard,
                     hi-colour, hi-res) is detected from the file size.
    scr2gif          Render a .scr to a GIF, animating the FLASH
                     attribute as a 2-frame loop (static if no FLASH).
    info             Show a summary of a .scr (mode, size, colours).

Options:
    -h, --help       Display this help and exit.
    --mode <m>       Target mode for png2scr (default std):
                       std        256x192, 8x8 attrs  -> 6912 bytes
                       hicolour   256x192, 8x1 attrs  -> 12288 bytes
                       hires      512x192 mono+colour -> 12289 bytes
    --level <n>      Normal (non-bright) colour level for rendering,
                     0-255 (default 215 = 0xD7). Bright stays 255.
    --palette <p>    Named level preset: standard (215, default),
                     classic (205), emulator (192).
    --scale <n>      Upscale scr2png / scr2gif output by factor n.
    --delay <cs>     FLASH frame delay for scr2gif, centiseconds
                     (default 32 = the hardware ~1.56 Hz rate).

Examples:
    scrtools png2scr factory.png factory.scr
    scrtools png2scr --mode hires logo512.png logo.scr
    scrtools png2scr --mode hicolour pic.png pic.scr
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
| 12288 | Timex hi-colour  | 6144 bitmap + 6144 attrs (8×1: one attr per scanline) |
| 12289 | Timex hi-res     | two 6144 banks (512×192 mono) + 1 colour byte |

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

## The palette

The Spectrum is an analogue machine; "normal" brightness is ≈85% of the bright
voltage, which different tools digitise to different RGB levels. scr-tools
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
  colours rather than being rejected.
* The rendered image is the canonical truth: `png → scr → png` is
  **pixel-identical** (at a matching `--level`), and `png2scr` output is a stable
  **fixed point**. On real Timex hi-res samples, `scr → png → scr` is even
  byte-identical. Note `scr → png → scr` is not byte-identical in general for
  attribute modes (the render can't recover an ink/paper swap, nor a colour a
  cell never paints).
* **scr2gif** animates the `FLASH` attribute as a looping 2-frame GIF (ink/paper
  swap every 320 ms, the hardware rate). GIF's indexed colour is a perfect fit
  for the 16-entry Spectrum palette. Screens with no FLASH cells produce a
  single static frame.

## Roadmap

* Direct ingestion from TAP `SCREEN$` blocks (pairs with `taput export-png`).
* ULAplus 64-register screens (6976 / 12352 / 12353 byte variants).

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
