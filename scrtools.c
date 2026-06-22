// scrtools - ZX Spectrum SCREEN$ (.scr) manipulation utility
//
// Part of the LOADZX toolset (see also: taput, tomato).
//
// Converts between PNG and the three common .scr screen formats:
//
//   * Standard      6912 bytes  256x192, 8x8 attributes  (the classic screen)
//   * Timex hi-colour 12288 b   256x192, 8x1 attributes  (one attr per scanline)
//   * Timex hi-res    12289 b   512x192 monochrome + 1 global colour byte
//
// PNG decoding is delegated to the vendored, public-domain stb_image.h. PNG
// encoding is hand-rolled (stored DEFLATE blocks, no zlib dependency) so the
// write path mirrors taput and keeps the build dependency-free.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// -- Vendored PNG decoder ------------------------------------------------
// stb_image.h is a single-header, public-domain image loader. We compile its
// implementation directly into this object file (STB_IMAGE_IMPLEMENTATION),
// restrict it to PNG (we never load anything else), and forbid its <stdio.h>
// loaders since we always hand it an in-memory buffer ourselves. The two
// pragmas below silence stb's own "unused static helper" warnings so our
// build stays clean under -Wall -Wextra without us touching vendored code.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO  // we feed our own buffer; no fopen() inside stb
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

typedef unsigned char byte;

#ifndef BUILD_TS
#define BUILD_TS 0
#endif
#define _STR(x) #x
#define STR(x) _STR(x)

//~~~~ SCR FORMAT ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Standard resolution screen:
//   - 256 x 192 pixels, 1 bit per pixel.
//   - Pixel area is 6144 bytes laid out in the Spectrum's interleaved order:
//       offset = (x>>3) + (y&7)*256 + ((y>>3)&7)*32 + (y>>6)*2048
//   - Attribute area is 768 bytes (32x24 cells of 8x8 pixels), one byte each:
//       bit7 FLASH, bit6 BRIGHT, bits5-3 PAPER (0-7), bits2-0 INK (0-7)
//   - Total 6912 bytes.
//
// Timex SCLD extended modes (selected at runtime via port 0xFF), stored as
// two 6144-byte blocks:
//   - Hi-colour (12288 bytes): block 1 = bitmap, block 2 = attributes at 8x1
//     resolution (one attribute byte per 8-pixel-wide, 1-pixel-tall strip).
//     Crucially, block 2 uses the SAME interleaved address scheme as block 1,
//     so the attribute for pixel (x,y) lives at the very same offset in block 2
//     as the pixel byte does in block 1.
//   - Hi-res (12288/12289 bytes): 512x192 monochrome. Even pixel columns come
//     from block 1, odd columns from block 2 (each block a normal interleaved
//     256x192 bitmap). A trailing byte (-> 12289) is the raw port-0xFF value;
//     its bits 3-5 are the global INK colour and PAPER = 7 - INK, both BRIGHT.
//
// Colour bits (per Spectrum hardware): bit0 BLUE, bit1 RED, bit2 GREEN.

#define SCR_W       256
#define SCR_H       192
#define SCR_PIXELS  6144
#define SCR_ATTRS   768
#define SCR_SIZE    (SCR_PIXELS + SCR_ATTRS)   // 6912, standard
#define TMX_SIZE    (SCR_PIXELS * 2)           // 12288, hi-colour / hi-res
#define TMX_HIRES   (TMX_SIZE + 1)             // 12289, hi-res with colour byte
#define HIRES_W     512
#define PAL_SIZE    64                         // ULAplus appends 64 GRB332 regs

// ATTR_AT: byte index (0..767) of the attribute cell covering pixel (x,y).
//   Cells are 8x8, laid out left-to-right, top-to-bottom: 32 per row, 24 rows.
//   (y>>3) = cell row, (x>>3) = cell column.
#define ATTR_AT(y, x) (((y) >> 3) * 32 + ((x) >> 3))

// PIX_OFF: byte index (0..6143) holding the 8 horizontal pixels at (x,y) in
// the Spectrum's famously non-linear bitmap layout. The display file is split
// into THREE thirds of 64 scanlines each (top/middle/bottom). Within a third
// the 8 character rows are interleaved by scanline, so consecutive bytes in
// memory are NOT consecutive on screen. Decoding the address:
//   (x>>3)           -> which of the 32 bytes across the line (8 px per byte)
//   (y&7)*256        -> scanline within an 8-px character row (0..7)
//   ((y>>3)&7)*32    -> character row within the current third (0..7)
//   (y>>6)*2048      -> which third of the screen (0..2)
// The most significant bit (0x80) of each byte is the leftmost pixel.
#define PIX_OFF(y, x) (((x) >> 3) + ((y) & 7) * 256 + (((y) >> 3) & 7) * 32 + ((y) >> 6) * 2048)

// Default render palette. Modern de-facto standard (Wikipedia / retrotechlab):
// normal level 0xD7 (215, ~85% of the measured ULA voltage ratio), bright 0xFF.
// Black is 0x000000 regardless of BRIGHT, which is why there are 15 distinct
// colours and not 16. Override the normal level with --level / --palette.
//   Common alternatives: 0xD8 (216, Lospec), 0xCD (205, older tools),
//   0xC0/0xBF (192/191, classic emulators such as Fuse / img2spec).
#define DEF_NORMAL  0xD7
#define DEF_BRIGHT  0xFF

//~~~~ CLI ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

static const char *Help =
    "SCRTOOLS " STR(BUILD_TS) " - ZX Spectrum SCREEN$ utility\n"
    "\n"
    "Usage:\n"
    "    scrtools command [options] FileIn [FileOut]\n"
    "\n"
    "Commands:\n"
    "    png2scr          Convert a PNG to a .scr (see --mode).\n"
    "    scr2png          Render a .scr to a PNG. The screen mode (standard,\n"
    "                     hi-colour, hi-res) is detected from the file size.\n"
    "    scr2gif          Render a .scr to a GIF, animating the FLASH\n"
    "                     attribute as a 2-frame loop (static if no FLASH).\n"
    "    info             Show a summary of a .scr (mode, size, colours).\n"
    "\n"
    "Options:\n"
    "    -h, --help       Display this help and exit.\n"
    "    --mode <m>       Target mode for png2scr (default std):\n"
    "                       std        256x192, 8x8 attrs  -> 6912 bytes\n"
    "                       hicolour   256x192, 8x1 attrs  -> 12288 bytes\n"
    "                       hires      512x192 mono+colour -> 12289 bytes\n"
    "    --ulaplus        png2scr: emit a ULAplus 64-colour screen (adds 64\n"
    "                     palette bytes -> 6976 / 12352). std and hicolour only.\n"
    "                     scr2png/scr2gif/info auto-detect ULAplus screens.\n"
    "    --level <n>      Normal (non-bright) colour level for rendering,\n"
    "                     0-255 (default 215 = 0xD7). Bright stays 255.\n"
    "    --palette <p>    Named level preset: standard (215, default),\n"
    "                     classic (205), emulator (192).\n"
    "    --dither         png2scr: Floyd-Steinberg dither the bitmap between\n"
    "                     each cell's two colours (helps shaded images).\n"
    "    --scale <n>      Upscale scr2png / scr2gif output by factor n.\n"
    "    --delay <cs>     FLASH frame delay for scr2gif, centiseconds\n"
    "                     (default 32 = the hardware ~1.56 Hz rate).\n"
    "\n"
    "Examples:\n"
    "    scrtools png2scr factory.png factory.scr\n"
    "    scrtools png2scr --mode hires logo512.png logo.scr\n"
    "    scrtools png2scr --mode hicolour pic.png pic.scr\n"
    "    scrtools png2scr --ulaplus photo.png photo.scr\n"
    "    scrtools scr2png factory.scr factory.png\n"
    "    scrtools scr2png --palette emulator factory.scr factory.png\n"
    "    scrtools scr2gif --scale 2 demo.scr demo.gif\n"
    "    scrtools info factory.scr\n";

enum Command { cmNone, cmPng2Scr, cmScr2Png, cmScr2Gif, cmInfo };
enum Mode    { mStd, mHiColour, mHiRes };

//~~~~ I/O helpers ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

static void die(const char *msg, const char *arg)
{
    if (arg) fprintf(stderr, "%s \"%s\"\n", msg, arg);
    else     fprintf(stderr, "%s\n", msg);
    exit(1);
}

static byte *readFile(const char *name, size_t *outLen)
{
    FILE *f = fopen(name, "rb");
    if (!f) die("Unable to open file", name);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); die("Cannot determine size of", name); }
    byte *buf = (byte *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); die("Memory allocation error", NULL); }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); die("Error reading", name); }
    fclose(f);
    *outLen = (size_t)n;
    return buf;
}

static void writeFile(const char *name, const byte *data, size_t len)
{
    FILE *f = fopen(name, "wb");
    if (!f) die("Unable to create file", name);
    if (fwrite(data, 1, len, f) != len) { fclose(f); die("Error writing", name); }
    fclose(f);
}

//~~~~ PNG encode (stored DEFLATE, no zlib) ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Ported from taput so the LOADZX tools share an identical, dependency-free
// PNG writer.

static uint32_t crc32buf(const byte *buf, size_t len, uint32_t crc)
{
    static uint32_t table[256];
    static int init = 0;
    if (!init)
    {
        for (uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xedb88320UL ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = 1;
    }
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

// Wrap raw data in a zlib stream using only stored (uncompressed) blocks. This
// sidesteps a real DEFLATE compressor; the PNGs are a touch larger but the code
// stays tiny and dependency-free, which suits 6KB-class screen images.
static byte *zlibStore(const byte *data, size_t len, size_t *outLen)
{
    size_t blocks = len / 65535 + 1;
    byte *out = (byte *)malloc(2 + len + blocks * 5 + 4);
    if (!out) return NULL;
    size_t p = 0;
    out[p++] = 0x78;            // CMF: deflate, 32K window
    out[p++] = 0x01;            // FLG
    size_t off = 0;
    while (off < len || len == 0)
    {
        size_t chunk = len - off;
        if (chunk > 65535) chunk = 65535;
        int final = (off + chunk >= len);
        out[p++] = final ? 1 : 0;       // BFINAL + BTYPE=00 (stored)
        out[p++] = chunk & 0xff;        // LEN  (little-endian)
        out[p++] = (chunk >> 8) & 0xff;
        out[p++] = ~chunk & 0xff;       // NLEN (one's complement of LEN)
        out[p++] = (~chunk >> 8) & 0xff;
        memcpy(out + p, data + off, chunk);
        p += chunk;
        off += chunk;
        if (len == 0) break;
    }
    // Adler-32 checksum of the uncompressed data closes the zlib stream.
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++)
    {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    out[p++] = (adler >> 24) & 0xff;
    out[p++] = (adler >> 16) & 0xff;
    out[p++] = (adler >> 8) & 0xff;
    out[p++] = adler & 0xff;
    *outLen = p;
    return out;
}

static void writePngChunk(FILE *f, const char *type, const byte *data, size_t len)
{
    byte hdr[4] = { (len >> 24) & 0xff, (len >> 16) & 0xff, (len >> 8) & 0xff, len & 0xff };
    fwrite(hdr, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    // CRC covers the chunk type AND its data (but not the length field).
    uint32_t c = crc32buf((const byte *)type, 4, 0xffffffffUL);
    c = crc32buf(data, len, c) ^ 0xffffffffUL;
    byte crc[4] = { (c >> 24) & 0xff, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff };
    fwrite(crc, 1, 4, f);
}

// Write an RGB truecolour PNG from a tightly packed w*h*3 buffer.
static void writeRgbPng(const char *name, const byte *rgb, int w, int h)
{
    // PNG scanlines are prefixed by a 1-byte filter selector; we use 0 (none)
    // and store the rows verbatim, leaving compression to zlibStore().
    size_t stride = (size_t)w * 3;
    size_t rawLen = (size_t)h * (1 + stride);
    byte *raw = (byte *)malloc(rawLen);
    if (!raw) die("Memory allocation error", NULL);
    size_t rp = 0;
    for (int y = 0; y < h; y++)
    {
        raw[rp++] = 0;          // filter type 0 (none)
        memcpy(raw + rp, rgb + (size_t)y * stride, stride);
        rp += stride;
    }
    size_t zlen;
    byte *zdata = zlibStore(raw, rawLen, &zlen);
    free(raw);
    if (!zdata) die("Memory allocation error", NULL);

    FILE *f = fopen(name, "wb");
    if (!f) die("Unable to create file", name);
    static const byte sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    byte ihdr[13] = {
        (w >> 24) & 0xff, (w >> 16) & 0xff, (w >> 8) & 0xff, w & 0xff,
        (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff,
        8, 2, 0, 0, 0    // 8-bit, truecolour RGB, no compression/filter/interlace
    };
    fwrite(sig, 1, 8, f);
    writePngChunk(f, "IHDR", ihdr, 13);
    writePngChunk(f, "IDAT", zdata, zlen);
    writePngChunk(f, "IEND", NULL, 0);
    fclose(f);
    free(zdata);
}

//~~~~ Colour helpers ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

// Expand a 3-bit Spectrum colour index (+ bright flag) to an RGB triple at the
// chosen normal level. The 3 bits are GRB: bit1=red, bit2=green, bit0=blue.
static void indexToRGB(int colour, int bright, int normal, byte *r, byte *g, byte *b)
{
    int v = bright ? DEF_BRIGHT : normal;
    *r = (colour & 2) ? v : 0;
    *g = (colour & 4) ? v : 0;
    *b = (colour & 1) ? v : 0;
}

// Classify a source RGB pixel into a Spectrum colour index 0-7 (bit0 blue,
// bit1 red, bit2 green) plus a bright vote. A channel is "on" if clearly lit
// and "bright" if near full intensity; the thresholds bracket every common
// normal level (0xBF, 0xC0, 0xCD, 0xD7, 0xD8) so authoring level doesn't matter.
static int classify(byte r, byte g, byte b, int *bright)
{
    int ron = r >= 0x60, gon = g >= 0x60, bon = b >= 0x60;
    int hi = (r >= 0xE0) || (g >= 0xE0) || (b >= 0xE0);
    *bright = hi;
    return (gon << 2) | (ron << 1) | bon;
}

//~~~~ ULAplus (GRB332 64-colour palette) ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// A ULAplus screen appends 64 palette registers, one byte each, in the layout
// GGGRRRBB: bits 7-5 green, 4-2 red, 1-0 blue. A 3-bit channel expands to 8
// bits by bit-replication (hmlhmlhm); the 2-bit blue first gains a synthesised
// 3rd low bit equal to (b1 | b0), per the official spec, then replicates too.
//
// When a palette is present FLASH and BRIGHT stop meaning flash/bright: together
// they select one of four 16-entry CLUT groups. Within a group, INK picks slots
// 0-7 and PAPER slots 8-15:
//     ink_index   = (FLASH<<5) | (BRIGHT<<4) |     INK         (group*16 + ink)
//     paper_index = (FLASH<<5) | (BRIGHT<<4) | 8 | PAPER       (group*16 + 8 + paper)
// A cell still shows only two colours, but each may be any of the 64 entries
// (provided both come from the same group). There is no hardware FLASH in this
// mode (the bit is repurposed).

static byte exp3(int x) { return (byte)((x << 5) | (x << 2) | (x >> 1)); }

// Decode one GRB332 register to 8-bit RGB.
static void plusToRGB(byte e, byte *r, byte *g, byte *b)
{
    int g3 = (e >> 5) & 7, r3 = (e >> 2) & 7;
    int b1 = (e >> 1) & 1, b0 = e & 1;
    int b3 = (b1 << 2) | (b0 << 1) | (b1 | b0);
    *r = exp3(r3); *g = exp3(g3); *b = exp3(b3);
}

// Nearest representable 3-bit channel level for an 8-bit value.
static int nearest3(int v)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 8; i++) { int d = exp3(i) - v; if (d < 0) d = -d; if (d < bd) { bd = d; best = i; } }
    return best;
}

// Snap an 8-bit RGB triple to the nearest GRB332 register byte.
static byte rgbToPlus(int r, int g, int b)
{
    int g3 = nearest3(g), r3 = nearest3(r), bestB = 0, bd = 1 << 30;
    for (int b2 = 0; b2 < 4; b2++)
    {
        int b1 = (b2 >> 1) & 1, b0 = b2 & 1;
        int lvl = exp3((b1 << 2) | (b0 << 1) | (b1 | b0));
        int d = lvl - b; if (d < 0) d = -d;
        if (d < bd) { bd = d; bestB = b2; }
    }
    return (byte)((g3 << 5) | (r3 << 2) | bestB);
}

// Squared RGB distance between two GRB332 registers (quantiser metric).
static int plusDist(byte a, byte b)
{
    byte ra, ga, ba, rb, gb, bb;
    plusToRGB(a, &ra, &ga, &ba); plusToRGB(b, &rb, &gb, &bb);
    int dr = ra - rb, dg = ga - gb, db = ba - bb;
    return dr * dr + dg * dg + db * db;
}

// Detected screen geometry plus whether a 64-byte ULAplus palette is appended.
typedef struct { enum Mode mode; int palette; int valid; } Detected;
static Detected detect(size_t len)
{
    Detected d = { mStd, 0, 1 };
    switch (len)
    {
        case SCR_SIZE:            d.mode = mStd;      d.palette = 0; break;  // 6912
        case SCR_SIZE + PAL_SIZE: d.mode = mStd;      d.palette = 1; break;  // 6976
        case TMX_SIZE:            d.mode = mHiColour; d.palette = 0; break;  // 12288
        case TMX_SIZE + PAL_SIZE: d.mode = mHiColour; d.palette = 1; break;  // 12352
        case TMX_HIRES:           d.mode = mHiRes;    d.palette = 0; break;  // 12289
        case TMX_HIRES + PAL_SIZE:d.mode = mHiRes;    d.palette = 1; break;  // 12353
        default: d.valid = 0; break;
    }
    return d;
}

// Resolve (attribute, pixel-on) to RGB. With a ULAplus CLUT present the FLASH/
// BRIGHT bits choose a group; otherwise the classic fixed palette is used.
static void resolveRGB(byte attr, int on, const byte *clut, int normal,
                       byte *r, byte *g, byte *b)
{
    if (clut)
    {
        int grp = (attr >> 6) & 3;
        int idx = on ? (grp * 16 + (attr & 7)) : (grp * 16 + 8 + ((attr >> 3) & 7));
        plusToRGB(clut[idx], r, g, b);
    }
    else
    {
        int colour = on ? (attr & 7) : ((attr >> 3) & 7);
        indexToRGB(colour, attr & 0x40, normal, r, g, b);
    }
}

#define BAD_SIZE_MSG "Unrecognised .scr size " \
    "(need 6912/6976, 12288/12352 or 12289/12353):"

//~~~~ scr2png (mode auto-detected by file size) ~~~~~~~~~~~~~~~~~~~~~~~~~~~//

// Splat one logical pixel into a scale x scale block of the output buffer.
static void putPixel(byte *rgb, int W, int x, int y, int scale, byte r, byte g, byte b)
{
    for (int sy = 0; sy < scale; sy++)
        for (int sx = 0; sx < scale; sx++)
        {
            size_t p = ((size_t)(y * scale + sy) * W + (x * scale + sx)) * 3;
            rgb[p] = r; rgb[p + 1] = g; rgb[p + 2] = b;
        }
}

static void cmdScr2Png(const char *in, const char *out, int normal, int scale)
{
    size_t len;
    byte *scr = readFile(in, &len);
    Detected d = detect(len);
    if (!d.valid) { free(scr); die(BAD_SIZE_MSG, in); }

    // ULAplus CLUT is the last 64 bytes. It is well-defined for the attribute
    // modes; for hi-res it is under-specified, so we render hi-res with its
    // fixed colour pair and leave the palette unapplied (but preserved on disk).
    const byte *clut = (d.palette && d.mode != mHiRes) ? scr + len - PAL_SIZE : NULL;

    int srcW = (d.mode == mHiRes) ? HIRES_W : SCR_W;
    int W = srcW * scale, H = SCR_H * scale;
    byte *rgb = (byte *)malloc((size_t)W * H * 3);
    if (!rgb) die("Memory allocation error", NULL);

    if (d.mode == mStd)
    {
        // -- Standard: per-pixel bit + per-8x8-cell attribute. --
        for (int y = 0; y < SCR_H; y++)
            for (int x = 0; x < SCR_W; x++)
            {
                // 8 pixels per byte, MSB-first: column x uses bit (7 - x%8).
                int on = (scr[PIX_OFF(y, x)] >> (7 - (x & 7))) & 1;
                byte attr = scr[SCR_PIXELS + ATTR_AT(y, x)];
                byte r, g, b;
                resolveRGB(attr, on, clut, normal, &r, &g, &b);
                putPixel(rgb, W, x, y, scale, r, g, b);
            }
    }
    else if (d.mode == mHiColour)
    {
        // -- Timex hi-colour: bitmap in block 1, 8x1 attributes in block 2 at
        //    the SAME interleaved offset, so attr(x,y) == block2[PIX_OFF]. --
        const byte *bmp = scr, *att = scr + SCR_PIXELS;
        for (int y = 0; y < SCR_H; y++)
            for (int x = 0; x < SCR_W; x++)
            {
                int o = PIX_OFF(y, x);
                int on = (bmp[o] >> (7 - (x & 7))) & 1;
                byte r, g, b;
                resolveRGB(att[o], on, clut, normal, &r, &g, &b);
                putPixel(rgb, W, x, y, scale, r, g, b);
            }
    }
    else
    {
        // -- Timex hi-res: 512x192 mono, even cols from block 1, odd from
        //    block 2. Colour is global: bits 3-5 of the trailing port byte are
        //    INK, PAPER = 7 - INK, both BRIGHT. --
        const byte *bank0 = scr, *bank1 = scr + SCR_PIXELS;
        int port = scr[TMX_SIZE];          // the 12289th byte
        int ink = (port >> 3) & 7, paper = 7 - ink;
        for (int y = 0; y < SCR_H; y++)
            for (int X = 0; X < HIRES_W; X++)
            {
                const byte *bank = (X & 1) ? bank1 : bank0;   // odd cols: block 2
                int col = X >> 1;                              // column within bank
                int on = (bank[PIX_OFF(y, col)] >> (7 - (col & 7))) & 1;
                byte r, g, b;
                indexToRGB(on ? ink : paper, 1 /*bright*/, normal, &r, &g, &b);
                putPixel(rgb, W, X, y, scale, r, g, b);
            }
    }

    writeRgbPng(out, rgb, W, H);
    free(rgb);
    free(scr);
    printf("Wrote %s (%dx%d%s)\n", out, W, H, clut ? ", ULAplus palette" : "");
}

//~~~~ png2scr helpers ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

// Decode a PNG to a forced-RGB buffer and validate its size against the target
// logical resolution (wantW x SCR_H). Integer multiples are accepted and the
// down-sample factor is returned in *f. Caller frees with stbi_image_free.
static byte *loadPngRGB(const char *in, int wantW, int *w, int *h, int *f)
{
    size_t flen;
    byte *fbuf = readFile(in, &flen);
    int comp;
    byte *px = stbi_load_from_memory(fbuf, (int)flen, w, h, &comp, 3);
    free(fbuf);
    if (!px) die("Cannot decode PNG:", in);
    if (*w % wantW != 0 || *h % SCR_H != 0 || *w / wantW != *h / SCR_H)
    {
        stbi_image_free(px);
        fprintf(stderr, "PNG must be %dx%d or an integer multiple of it\n", wantW, SCR_H);
        exit(1);
    }
    *f = *w / wantW;
    return px;
}

// Sample the (logical) pixel (lx,ly) from an up-scaled source buffer.
static const byte *samplePx(const byte *px, int w, int f, int lx, int ly)
{
    return px + ((size_t)(ly * f) * w + (lx * f)) * 3;
}

//~~~~ Higher-quality quantisation (2-means, median-cut, dithering) ~~~~~~~//

// Two representative GRB332 colours for a cw x ch cell, via k=2 Lloyd's
// algorithm (better than picking the two most frequent for shaded cells). The
// more populous cluster becomes PAPER, the other INK (equal if single-colour).
static void cellTwoColours(const byte *px, int w, int f, int x0, int y0,
                           int cw, int ch, int *ink, int *paper)
{
    int n = cw * ch;
    const byte *p0 = samplePx(px, w, f, x0, y0);
    double m0[3] = { p0[0], p0[1], p0[2] }, m1[3];
    // Seed the second centroid at the pixel farthest from the first.
    int bd = -1; m1[0] = m0[0]; m1[1] = m0[1]; m1[2] = m0[2];
    for (int j = 0; j < n; j++)
    {
        const byte *p = samplePx(px, w, f, x0 + j % cw, y0 + j / cw);
        int dr = p[0] - (int)m0[0], dg = p[1] - (int)m0[1], db = p[2] - (int)m0[2];
        int d = dr * dr + dg * dg + db * db;
        if (d > bd) { bd = d; m1[0] = p[0]; m1[1] = p[1]; m1[2] = p[2]; }
    }
    int n0 = n, n1 = 0;
    for (int it = 0; it < 8; it++)
    {
        double s0[3] = {0,0,0}, s1[3] = {0,0,0}; n0 = 0; n1 = 0;
        for (int j = 0; j < n; j++)
        {
            const byte *p = samplePx(px, w, f, x0 + j % cw, y0 + j / cw);
            double d0 = 0, d1 = 0;
            for (int c = 0; c < 3; c++) { double a = p[c]-m0[c], b = p[c]-m1[c]; d0 += a*a; d1 += b*b; }
            if (d0 <= d1) { s0[0]+=p[0]; s0[1]+=p[1]; s0[2]+=p[2]; n0++; }
            else          { s1[0]+=p[0]; s1[1]+=p[1]; s1[2]+=p[2]; n1++; }
        }
        if (n0) for (int c = 0; c < 3; c++) m0[c] = s0[c] / n0;
        if (n1) for (int c = 0; c < 3; c++) m1[c] = s1[c] / n1;
    }
    byte a = rgbToPlus((int)(m0[0]+0.5), (int)(m0[1]+0.5), (int)(m0[2]+0.5));
    byte b = rgbToPlus((int)(m1[0]+0.5), (int)(m1[1]+0.5), (int)(m1[2]+0.5));
    if (n0 >= n1) { *paper = a; *ink = n1 ? b : a; }
    else          { *paper = b; *ink = a; }
}

// Reduce a weighted set of GRB332 colours to <=k representatives (GRB332 bytes,
// written to out, count returned). Classic median-cut: repeatedly split the box
// with the largest channel spread at its weighted median, then average each box.
static int medianCut(const byte *cols, const int *wts, int n, int k, byte *out)
{
    if (n <= 0) return 0;
    if (k > 8) k = 8;
    int *R = (int*)malloc(sizeof(int)*n), *G = (int*)malloc(sizeof(int)*n);
    int *B = (int*)malloc(sizeof(int)*n), *W = (int*)malloc(sizeof(int)*n);
    int *idx = (int*)malloc(sizeof(int)*n);
    if (!R||!G||!B||!W||!idx) die("Memory allocation error", NULL);
    for (int i = 0; i < n; i++)
    { byte r,g,b; plusToRGB(cols[i], &r,&g,&b); R[i]=r; G[i]=g; B[i]=b; W[i]=wts[i]; idx[i]=i; }

    int bl[8], bh[8], nb = 1; bl[0] = 0; bh[0] = n;
    while (nb < k)
    {
        int best = -1; long bestSpread = -1; int bestAxis = 0;
        for (int bi = 0; bi < nb; bi++)
        {
            if (bh[bi] - bl[bi] <= 1) continue;
            int mn[3] = {255,255,255}, mx[3] = {0,0,0};
            for (int j = bl[bi]; j < bh[bi]; j++)
            { int p = idx[j]; int v[3]={R[p],G[p],B[p]};
              for (int c=0;c<3;c++){ if(v[c]<mn[c])mn[c]=v[c]; if(v[c]>mx[c])mx[c]=v[c]; } }
            for (int c = 0; c < 3; c++)
            { long sp = mx[c]-mn[c]; if (sp > bestSpread) { bestSpread = sp; best = bi; bestAxis = c; } }
        }
        if (best < 0 || bestSpread <= 0) break;
        // Insertion-sort this box's indices along the chosen axis.
        int *chan = bestAxis == 0 ? R : bestAxis == 1 ? G : B;
        for (int a = bl[best]+1; a < bh[best]; a++)
        { int t = idx[a], j = a-1; while (j >= bl[best] && chan[idx[j]] > chan[t]) { idx[j+1]=idx[j]; j--; } idx[j+1]=t; }
        // Split at the weighted median.
        long total = 0; for (int j = bl[best]; j < bh[best]; j++) total += W[idx[j]];
        long acc = 0; int mid = bl[best]+1;
        for (int j = bl[best]; j < bh[best]-1; j++) { acc += W[idx[j]]; if (acc*2 >= total) { mid = j+1; break; } }
        bl[nb] = mid; bh[nb] = bh[best]; bh[best] = mid; nb++;
    }
    for (int bi = 0; bi < nb; bi++)
    {
        double sr=0,sg=0,sb=0; long sw=0;
        for (int j = bl[bi]; j < bh[bi]; j++)
        { int p = idx[j]; sr += (double)R[p]*W[p]; sg += (double)G[p]*W[p]; sb += (double)B[p]*W[p]; sw += W[p]; }
        if (!sw) sw = 1;
        out[bi] = rgbToPlus((int)(sr/sw+0.5), (int)(sg/sw+0.5), (int)(sb/sw+0.5));
    }
    free(R); free(G); free(B); free(W); free(idx);
    return nb;
}

// Floyd-Steinberg error buffers (two rolling rows of RGB residual).
static double gErr[2][SCR_W][3];

// Emit the 6144-byte bitmap for a 256-wide attribute screen. Each pixel is set
// to INK or PAPER by nearest colour to (source + diffused error). cellH is 8
// for standard, 1 for hi-colour; cell colours are passed as packed RGB arrays
// indexed by (y/cellH)*32 + x/8.
static void emitBitmap256(const byte *px, int w, int f, const byte *cellInkRGB,
                          const byte *cellPaperRGB, int cellH, byte *bmp, int dither)
{
    memset(gErr, 0, sizeof gErr);
    for (int y = 0; y < SCR_H; y++)
    {
        double (*cur)[3] = gErr[y & 1], (*nxt)[3] = gErr[(y + 1) & 1];
        for (int x = 0; x < SCR_W; x++) for (int c = 0; c < 3; c++) nxt[x][c] = 0;
        int cellRow = y / cellH;
        int bits = 0;
        for (int x = 0; x < SCR_W; x++)
        {
            int cell = cellRow * 32 + (x >> 3);
            const byte *ink = cellInkRGB + cell * 3, *pap = cellPaperRGB + cell * 3;
            const byte *p = samplePx(px, w, f, x, y);
            double t[3];
            for (int c = 0; c < 3; c++) t[c] = p[c] + (dither ? cur[x][c] : 0);
            double di = 0, dp = 0;
            for (int c = 0; c < 3; c++) { double a = t[c]-ink[c], b = t[c]-pap[c]; di += a*a; dp += b*b; }
            const byte *chosen = (di <= dp) ? ink : pap;
            if (di <= dp) bits |= (0x80 >> (x & 7));
            if (dither)
            {
                for (int c = 0; c < 3; c++)
                {
                    double e = t[c] - chosen[c];
                    if (x + 1 < SCR_W) cur[x+1][c] += e * 7.0/16;
                    if (x > 0) nxt[x-1][c] += e * 3.0/16;
                    nxt[x][c] += e * 5.0/16;
                    if (x + 1 < SCR_W) nxt[x+1][c] += e * 1.0/16;
                }
            }
            if ((x & 7) == 7) { bmp[PIX_OFF(y, x & ~7)] = (byte)bits; bits = 0; }
        }
    }
}

//~~~~ png2scr: standard (8x8 attributes) ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

static void cmdPng2ScrStd(const char *in, const char *out, int dither)
{
    int w, h, f;
    byte *px = loadPngRGB(in, SCR_W, &w, &h, &f);
    byte scr[SCR_SIZE];
    memset(scr, 0, sizeof scr);

    // Resolve one INK / PAPER / BRIGHT per 8x8 cell, recording each cell's two
    // colours as RGB for the (optionally dithered) bitmap pass.
    byte cellInk[24 * 32 * 3], cellPaper[24 * 32 * 3];
    for (int cy = 0; cy < 24; cy++)
        for (int cx = 0; cx < 32; cx++)
        {
            // Histogram the 8 colour indices over the cell's 64 pixels, and
            // count how many sit at full intensity (the bright vote).
            int hist[8] = {0}, brightVotes = 0;
            for (int py = 0; py < 8; py++)
                for (int pxl = 0; pxl < 8; pxl++)
                {
                    const byte *p = samplePx(px, w, f, cx * 8 + pxl, cy * 8 + py);
                    int br, idx = classify(p[0], p[1], p[2], &br);
                    hist[idx]++; brightVotes += br;
                }
            // PAPER = most frequent colour, INK = runner-up (cosmetic choice
            // for a still image; the same pixels are drawn either way).
            int paper = 0;
            for (int c = 1; c < 8; c++) if (hist[c] > hist[paper]) paper = c;
            int best2 = -1;
            for (int c = 0; c < 8; c++)
                if (c != paper && hist[c] > 0 && (best2 < 0 || hist[c] > hist[best2]))
                    best2 = c;
            int ink = (best2 < 0) ? paper : best2;      // single-colour cell
            // BRIGHT shared by the whole cell: any full-intensity pixel wins
            // (a black pixel is ambiguous, so majority would lose sparse cells).
            int bright = (brightVotes > 0);

            int cell = cy * 32 + cx;
            scr[SCR_PIXELS + cell] = (byte)((bright << 6) | (paper << 3) | ink);
            indexToRGB(ink,   bright, DEF_NORMAL, &cellInk[cell*3],   &cellInk[cell*3+1],   &cellInk[cell*3+2]);
            indexToRGB(paper, bright, DEF_NORMAL, &cellPaper[cell*3], &cellPaper[cell*3+1], &cellPaper[cell*3+2]);
        }

    emitBitmap256(px, w, f, cellInk, cellPaper, 8, scr, dither);
    stbi_image_free(px);
    writeFile(out, scr, SCR_SIZE);
    printf("Wrote %s (%d bytes, standard%s)\n", out, SCR_SIZE, dither ? ", dithered" : "");
}

//~~~~ png2scr: Timex hi-colour (8x1 attributes) ~~~~~~~~~~~~~~~~~~~~~~~~~~~//

static void cmdPng2ScrHiColour(const char *in, const char *out, int dither)
{
    int w, h, f;
    byte *px = loadPngRGB(in, SCR_W, &w, &h, &f);
    byte *scr = (byte *)calloc(TMX_SIZE, 1);
    if (!scr) die("Memory allocation error", NULL);
    byte *att = scr + SCR_PIXELS;

    // Same idea as standard, but a "cell" is now a single 8x1 strip: one
    // attribute per scanline per 8-pixel column. Bitmap byte and attribute byte
    // share the interleaved offset PIX_OFF(y, cx*8).
    byte *cellInk = (byte *)malloc((size_t)SCR_H * 32 * 3);
    byte *cellPaper = (byte *)malloc((size_t)SCR_H * 32 * 3);
    if (!cellInk || !cellPaper) die("Memory allocation error", NULL);
    for (int y = 0; y < SCR_H; y++)
        for (int cx = 0; cx < 32; cx++)
        {
            int hist[8] = {0}, brightVotes = 0;
            for (int pxl = 0; pxl < 8; pxl++)
            {
                const byte *p = samplePx(px, w, f, cx * 8 + pxl, y);
                int br, idx = classify(p[0], p[1], p[2], &br);
                hist[idx]++; brightVotes += br;
            }
            int paper = 0;
            for (int c = 1; c < 8; c++) if (hist[c] > hist[paper]) paper = c;
            int best2 = -1;
            for (int c = 0; c < 8; c++)
                if (c != paper && hist[c] > 0 && (best2 < 0 || hist[c] > hist[best2]))
                    best2 = c;
            int ink = (best2 < 0) ? paper : best2;
            int bright = (brightVotes > 0);

            int cell = y * 32 + cx;
            att[PIX_OFF(y, cx * 8)] = (byte)((bright << 6) | (paper << 3) | ink);
            indexToRGB(ink,   bright, DEF_NORMAL, &cellInk[cell*3],   &cellInk[cell*3+1],   &cellInk[cell*3+2]);
            indexToRGB(paper, bright, DEF_NORMAL, &cellPaper[cell*3], &cellPaper[cell*3+1], &cellPaper[cell*3+2]);
        }

    emitBitmap256(px, w, f, cellInk, cellPaper, 1, scr, dither);
    stbi_image_free(px);
    writeFile(out, scr, TMX_SIZE);
    free(scr); free(cellInk); free(cellPaper);
    printf("Wrote %s (%d bytes, Timex hi-colour%s)\n", out, TMX_SIZE, dither ? ", dithered" : "");
}

//~~~~ png2scr: Timex hi-res (512x192 mono + colour byte) ~~~~~~~~~~~~~~~~~~//

static double gErrHi[2][HIRES_W][3];

static void cmdPng2ScrHiRes(const char *in, const char *out, int dither)
{
    int w, h, f;
    byte *px = loadPngRGB(in, HIRES_W, &w, &h, &f);

    // Hi-res has ONE global colour pair for the whole screen. Histogram every
    // pixel, then pick the two dominant colours. PAPER is the more common
    // (background); INK the other. The hardware constrains PAPER to 7 - INK, so
    // if the runner-up isn't the exact complement we snap to it and warn.
    int hist[8] = {0};
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < HIRES_W; x++)
        {
            const byte *p = samplePx(px, w, f, x, y);
            int br, idx = classify(p[0], p[1], p[2], &br);
            hist[idx]++;
        }
    int paper = 0;
    for (int c = 1; c < 8; c++) if (hist[c] > hist[paper]) paper = c;
    int ink = 7 - paper;                         // hardware: PAPER = 7 - INK
    int best2 = -1;
    for (int c = 0; c < 8; c++)
        if (c != paper && hist[c] > 0 && (best2 < 0 || hist[c] > hist[best2]))
            best2 = c;
    if (best2 >= 0 && best2 != ink)
        fprintf(stderr, "Warning: hi-res needs INK = 7 - PAPER; snapping "
                        "colour %d to %d\n", best2, ink);

    byte *scr = (byte *)calloc(TMX_HIRES, 1);
    if (!scr) die("Memory allocation error", NULL);
    byte *bank0 = scr, *bank1 = scr + SCR_PIXELS;

    // The two render colours (both BRIGHT) drive nearest-colour / dithering.
    byte ic[3], pc[3];
    indexToRGB(ink, 1, DEF_NORMAL, &ic[0], &ic[1], &ic[2]);
    indexToRGB(paper, 1, DEF_NORMAL, &pc[0], &pc[1], &pc[2]);

    // Weave the 512-wide image into the two interleaved 256-wide banks: even
    // output columns -> bank 0, odd -> bank 1, with optional FS error diffusion.
    memset(gErrHi, 0, sizeof gErrHi);
    for (int y = 0; y < SCR_H; y++)
    {
        double (*cur)[3] = gErrHi[y & 1], (*nxt)[3] = gErrHi[(y + 1) & 1];
        for (int X = 0; X < HIRES_W; X++) for (int c = 0; c < 3; c++) nxt[X][c] = 0;
        for (int X = 0; X < HIRES_W; X++)
        {
            const byte *p = samplePx(px, w, f, X, y);
            double t[3]; for (int c = 0; c < 3; c++) t[c] = p[c] + (dither ? cur[X][c] : 0);
            double di = 0, dp = 0;
            for (int c = 0; c < 3; c++) { double a = t[c]-ic[c], b = t[c]-pc[c]; di += a*a; dp += b*b; }
            const byte *chosen = (di <= dp) ? ic : pc;
            if (di <= dp)
            {
                byte *bank = (X & 1) ? bank1 : bank0;
                int col = X >> 1;
                bank[PIX_OFF(y, col)] |= (0x80 >> (col & 7));
            }
            if (dither)
                for (int c = 0; c < 3; c++)
                {
                    double e = t[c] - chosen[c];
                    if (X + 1 < HIRES_W) cur[X+1][c] += e * 7.0/16;
                    if (X > 0) nxt[X-1][c] += e * 3.0/16;
                    nxt[X][c] += e * 5.0/16;
                    if (X + 1 < HIRES_W) nxt[X+1][c] += e * 1.0/16;
                }
        }
    }
    // Trailing colour byte = raw port-0xFF value: mode 0x06 (hi-res) in bits
    // 0-2, INK in bits 3-5. Emulators store and re-read exactly this byte.
    scr[TMX_SIZE] = (byte)(0x06 | (ink << 3));

    stbi_image_free(px);
    writeFile(out, scr, TMX_HIRES);
    free(scr);
    printf("Wrote %s (%d bytes, Timex hi-res, ink=%d paper=%d%s)\n",
           out, TMX_HIRES, ink, paper, dither ? ", dithered" : "");
}

//~~~~ png2scr: ULAplus quantiser ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Turning a truecolour PNG into a ULAplus screen is a constrained quantisation:
// each cell still shows only two colours, but they may be any of 64 — provided
// both come from the SAME 16-entry group (8 ink + 8 paper). The pipeline is:
//   1. per cell, snap pixels to GRB332 and take the two dominant colours;
//   2. fit those (ink,paper) pairs into 4 groups and build the 64-reg CLUT;
//   3. re-emit each cell's attribute + bitmap against its assigned palette.

typedef struct { int ink, paper; } CellPair;   // GRB332 register bytes (0..255)

// Build the 64-entry CLUT and assign each cell a group + ink/paper slot.
static void ulaQuantise(const CellPair *cells, int n, byte *clut,
                        byte *outGroup, byte *outInk, byte *outPaper)
{
    memset(clut, 0, PAL_SIZE);

    // --- Fast path: if the whole image needs <=8 ink and <=8 paper colours it
    //     all fits one group and the encode is lossless. ---
    int inkList[8], paperList[8], ni = 0, np = 0, fits = 1;
    for (int i = 0; i < n; i++)
    {
        int e = cells[i].ink, found = 0;
        for (int k = 0; k < ni; k++) if (inkList[k] == e) { found = 1; break; }
        if (!found) { if (ni == 8) { fits = 0; break; } inkList[ni++] = e; }
        e = cells[i].paper; found = 0;
        for (int k = 0; k < np; k++) if (paperList[k] == e) { found = 1; break; }
        if (!found) { if (np == 8) { fits = 0; break; } paperList[np++] = e; }
    }
    if (fits)
    {
        for (int k = 0; k < ni; k++) clut[k] = (byte)inkList[k];
        for (int k = 0; k < np; k++) clut[8 + k] = (byte)paperList[k];
        for (int i = 0; i < n; i++)
        {
            int ii = 0, pp = 0;
            for (int k = 0; k < ni; k++) if (inkList[k] == cells[i].ink) ii = k;
            for (int k = 0; k < np; k++) if (paperList[k] == cells[i].paper) pp = k;
            outGroup[i] = 0; outInk[i] = (byte)ii; outPaper[i] = (byte)pp;
        }
        return;
    }

    // --- Slow path: k-means(4) over a 6-D (inkRGB, paperRGB) feature. ---
    int (*feat)[6] = (int (*)[6])malloc(sizeof(*feat) * n);
    int *assign = (int *)malloc(sizeof(int) * n);
    if (!feat || !assign) die("Memory allocation error", NULL);
    for (int i = 0; i < n; i++)
    {
        byte r, g, b;
        plusToRGB((byte)cells[i].ink, &r, &g, &b);
        feat[i][0] = r; feat[i][1] = g; feat[i][2] = b;
        plusToRGB((byte)cells[i].paper, &r, &g, &b);
        feat[i][3] = r; feat[i][4] = g; feat[i][5] = b;
        assign[i] = 0;
    }
    // Deterministic spread initialisation (no RNG -> reproducible output).
    double cen[4][6];
    for (int k = 0; k < 4; k++)
    {
        int idx = (int)((long)k * (n - 1) / 3);
        for (int dd = 0; dd < 6; dd++) cen[k][dd] = feat[idx][dd];
    }
    for (int iter = 0; iter < 24; iter++)
    {
        for (int i = 0; i < n; i++)
        {
            double bd = 1e30; int bk = 0;
            for (int k = 0; k < 4; k++)
            {
                double s = 0;
                for (int dd = 0; dd < 6; dd++) { double df = feat[i][dd] - cen[k][dd]; s += df * df; }
                if (s < bd) { bd = s; bk = k; }
            }
            assign[i] = bk;
        }
        double sum[4][6] = {{0}}; int cnt[4] = {0};
        for (int i = 0; i < n; i++)
        {
            int k = assign[i]; cnt[k]++;
            for (int dd = 0; dd < 6; dd++) sum[k][dd] += feat[i][dd];
        }
        for (int k = 0; k < 4; k++)
            if (cnt[k]) for (int dd = 0; dd < 6; dd++) cen[k][dd] = sum[k][dd] / cnt[k];
            else { int idx = (int)((long)k * (n - 1) / 3); for (int dd = 0; dd < 6; dd++) cen[k][dd] = feat[idx][dd]; }
    }

    // Reduce each group's colours to its 8 ink + 8 paper slots by median-cut
    // (better tonal coverage than plain popularity, which drops spread colours).
    for (int k = 0; k < 4; k++)
    {
        int ih[256] = {0}, ph[256] = {0};
        for (int i = 0; i < n; i++) if (assign[i] == k) { ih[cells[i].ink]++; ph[cells[i].paper]++; }
        byte icol[256]; int iwt[256], inc = 0, pnc = 0;
        byte pcol[256]; int pwt[256];
        for (int c = 0; c < 256; c++) if (ih[c]) { icol[inc] = (byte)c; iwt[inc++] = ih[c]; }
        for (int c = 0; c < 256; c++) if (ph[c]) { pcol[pnc] = (byte)c; pwt[pnc++] = ph[c]; }
        medianCut(icol, iwt, inc, 8, &clut[k * 16]);
        medianCut(pcol, pwt, pnc, 8, &clut[k * 16 + 8]);
    }
    // Map each cell's colours to the nearest kept slot in its group.
    for (int i = 0; i < n; i++)
    {
        int k = assign[i], bi = 0, bid = 1 << 30, bp = 0, bpd = 1 << 30;
        for (int s = 0; s < 8; s++) { int dd = plusDist((byte)cells[i].ink, clut[k*16+s]);   if (dd < bid) { bid = dd; bi = s; } }
        for (int s = 0; s < 8; s++) { int dd = plusDist((byte)cells[i].paper, clut[k*16+8+s]); if (dd < bpd) { bpd = dd; bp = s; } }
        outGroup[i] = (byte)k; outInk[i] = (byte)bi; outPaper[i] = (byte)bp;
    }
    free(feat); free(assign);
}

// Decode each cell's chosen palette entries to RGB so the dithered emitter can
// pick ink-or-paper per pixel against the real colours.
static void ulaCellRGB(const byte *clut, const byte *grp, const byte *ii,
                       const byte *pp, int n, byte *cellInk, byte *cellPaper)
{
    for (int i = 0; i < n; i++)
    {
        int g = grp[i];
        plusToRGB(clut[g*16 + ii[i]],     &cellInk[i*3],   &cellInk[i*3+1],   &cellInk[i*3+2]);
        plusToRGB(clut[g*16 + 8 + pp[i]], &cellPaper[i*3], &cellPaper[i*3+1], &cellPaper[i*3+2]);
    }
}

static void cmdPng2ScrUlaStd(const char *in, const char *out, int dither)
{
    int w, h, f;
    byte *px = loadPngRGB(in, SCR_W, &w, &h, &f);
    CellPair cells[768];
    for (int cy = 0; cy < 24; cy++)
        for (int cx = 0; cx < 32; cx++)
        {
            int ink, paper;
            cellTwoColours(px, w, f, cx * 8, cy * 8, 8, 8, &ink, &paper);
            cells[cy * 32 + cx].ink = ink; cells[cy * 32 + cx].paper = paper;
        }

    byte clut[PAL_SIZE], grp[768], ii[768], pp[768];
    ulaQuantise(cells, 768, clut, grp, ii, pp);

    byte cellInk[768 * 3], cellPaper[768 * 3];
    ulaCellRGB(clut, grp, ii, pp, 768, cellInk, cellPaper);

    byte *scr = (byte *)calloc(SCR_SIZE + PAL_SIZE, 1);
    if (!scr) die("Memory allocation error", NULL);
    for (int idx = 0; idx < 768; idx++)
        scr[SCR_PIXELS + idx] = (byte)((grp[idx] << 6) | (pp[idx] << 3) | ii[idx]);
    emitBitmap256(px, w, f, cellInk, cellPaper, 8, scr, dither);
    memcpy(scr + SCR_SIZE, clut, PAL_SIZE);
    stbi_image_free(px);
    writeFile(out, scr, SCR_SIZE + PAL_SIZE);
    free(scr);
    printf("Wrote %s (%d bytes, standard + ULAplus%s)\n",
           out, SCR_SIZE + PAL_SIZE, dither ? ", dithered" : "");
}

static void cmdPng2ScrUlaHiColour(const char *in, const char *out, int dither)
{
    int w, h, f;
    byte *px = loadPngRGB(in, SCR_W, &w, &h, &f);
    int n = SCR_H * 32;                       // 6144 strips (8x1)
    CellPair *cells = (CellPair *)malloc(sizeof(CellPair) * n);
    if (!cells) die("Memory allocation error", NULL);
    for (int y = 0; y < SCR_H; y++)
        for (int cx = 0; cx < 32; cx++)
        {
            int ink, paper;
            cellTwoColours(px, w, f, cx * 8, y, 8, 1, &ink, &paper);
            cells[y * 32 + cx].ink = ink; cells[y * 32 + cx].paper = paper;
        }

    byte clut[PAL_SIZE];
    byte *grp = (byte *)malloc(n), *ii = (byte *)malloc(n), *pp = (byte *)malloc(n);
    if (!grp || !ii || !pp) die("Memory allocation error", NULL);
    ulaQuantise(cells, n, clut, grp, ii, pp);

    byte *cellInk = (byte *)malloc((size_t)n * 3), *cellPaper = (byte *)malloc((size_t)n * 3);
    if (!cellInk || !cellPaper) die("Memory allocation error", NULL);
    ulaCellRGB(clut, grp, ii, pp, n, cellInk, cellPaper);

    byte *scr = (byte *)calloc(TMX_SIZE + PAL_SIZE, 1);
    if (!scr) die("Memory allocation error", NULL);
    byte *att = scr + SCR_PIXELS;
    for (int y = 0; y < SCR_H; y++)
        for (int cx = 0; cx < 32; cx++)
        {
            int idx = y * 32 + cx;
            att[PIX_OFF(y, cx * 8)] = (byte)((grp[idx] << 6) | (pp[idx] << 3) | ii[idx]);
        }
    emitBitmap256(px, w, f, cellInk, cellPaper, 1, scr, dither);
    memcpy(scr + TMX_SIZE, clut, PAL_SIZE);
    free(cellInk); free(cellPaper);
    stbi_image_free(px);
    writeFile(out, scr, TMX_SIZE + PAL_SIZE);
    free(scr); free(cells); free(grp); free(ii); free(pp);
    printf("Wrote %s (%d bytes, Timex hi-colour + ULAplus%s)\n",
           out, TMX_SIZE + PAL_SIZE, dither ? ", dithered" : "");
}

//~~~~ scr2gif (animate the FLASH attribute) ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// The Spectrum's FLASH bit swaps a cell's INK and PAPER ~1.56 times a second
// (every 16 frames at 50 Hz, i.e. 32 centiseconds per state). A GIF is an
// indexed-colour format, which suits the 15-colour Spectrum perfectly: we emit
// a global 16-entry palette and two full frames (normal / swapped) that loop
// forever. If a screen has no FLASH cells we emit a single static frame.

// A small growable byte buffer for assembling the GIF in memory.
typedef struct { byte *p; size_t n, cap; } Buf;
static void bufByte(Buf *b, byte v)
{
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 1024; b->p = (byte *)realloc(b->p, b->cap); if (!b->p) die("Memory allocation error", NULL); }
    b->p[b->n++] = v;
}
static void bufLE16(Buf *b, int v) { bufByte(b, v & 0xff); bufByte(b, (v >> 8) & 0xff); }
static void bufMem(Buf *b, const void *d, size_t n) { for (size_t i = 0; i < n; i++) bufByte(b, ((const byte *)d)[i]); }

// --- GIF LZW compressor -------------------------------------------------
// Standard variable-width LZW with a small open-addressing dictionary. Output
// is the raw LSB-first code bitstream; the caller chops it into <=255-byte
// sub-blocks. Correctness is verified end-to-end by decoding the GIF with a
// real decoder (PIL) in the regression suite.
#define LZW_HASH 8192
static int gHashKey[LZW_HASH];
static int gHashVal[LZW_HASH];

static void lzwReset(void)
{
    for (int i = 0; i < LZW_HASH; i++) gHashKey[i] = -1;
}
static int lzwFind(int key)
{
    int h = ((key >> 12) ^ key) & (LZW_HASH - 1);
    while (gHashKey[h] != -1)
    {
        if (gHashKey[h] == key) return gHashVal[h];
        h = (h + 1) & (LZW_HASH - 1);
    }
    return -1;
}
static void lzwInsert(int key, int val)
{
    int h = ((key >> 12) ^ key) & (LZW_HASH - 1);
    while (gHashKey[h] != -1) h = (h + 1) & (LZW_HASH - 1);
    gHashKey[h] = key; gHashVal[h] = val;
}

static void lzwEncode(Buf *out, const byte *idx, size_t n, int minCode)
{
    int clear = 1 << minCode, end = clear + 1;
    int codeSize = minCode + 1, next = clear + 2;
    unsigned acc = 0; int accBits = 0;
    // putBits, LSB-first, flushing whole bytes as they fill.
    #define PUT(code) do { acc |= (unsigned)(code) << accBits; accBits += codeSize; \
        while (accBits >= 8) { bufByte(out, acc & 0xff); acc >>= 8; accBits -= 8; } } while (0)

    lzwReset();
    PUT(clear);
    int prefix = idx[0];
    for (size_t i = 1; i < n; i++)
    {
        int c = idx[i];
        int key = (prefix << 8) | c;
        int found = lzwFind(key);
        if (found >= 0) { prefix = found; continue; }
        PUT(prefix);
        if (next < 4096)
        {
            lzwInsert(key, next++);
            if (next > (1 << codeSize) && codeSize < 12) codeSize++;
        }
        else
        {
            PUT(clear);                 // dictionary full: reset in lockstep
            lzwReset();
            codeSize = minCode + 1; next = clear + 2;
        }
        prefix = c;
    }
    PUT(prefix);
    PUT(end);
    if (accBits > 0) bufByte(out, acc & 0xff);   // flush remaining bits
    #undef PUT
}

// Append an LZW code stream as GIF image data: min-code-size byte, then
// length-prefixed sub-blocks of at most 255 bytes, then a 0 terminator.
static void gifImageData(Buf *gif, const byte *idx, size_t n, int minCode)
{
    bufByte(gif, (byte)minCode);
    Buf lzw = {0};
    lzwEncode(&lzw, idx, n, minCode);
    size_t off = 0;
    while (off < lzw.n)
    {
        size_t chunk = lzw.n - off; if (chunk > 255) chunk = 255;
        bufByte(gif, (byte)chunk);
        bufMem(gif, lzw.p + off, chunk);
        off += chunk;
    }
    bufByte(gif, 0x00);     // block terminator
    free(lzw.p);
}

// Build a logical-resolution palette-index frame for the given screen. swap
// toggles INK/PAPER in FLASH cells. *hasFlash reports whether any FLASH bit is
// set. Indices are 0-15: low 3 bits colour, bit 3 = bright. Returns malloc'd
// buffer of *w * *h bytes (caller frees).
static byte *sceneIndices(const byte *scr, size_t len, int swap, const byte *clut,
                          int *w, int *h, int *hasFlash)
{
    *hasFlash = 0;
    if (len == TMX_HIRES || len == TMX_HIRES + PAL_SIZE)
    {
        // Hi-res has no FLASH; one global bright ink/paper pair.
        *w = HIRES_W; *h = SCR_H;
        byte *out = (byte *)malloc((size_t)HIRES_W * SCR_H);
        if (!out) die("Memory allocation error", NULL);
        const byte *b0 = scr, *b1 = scr + SCR_PIXELS;
        int port = scr[TMX_SIZE], ink = (port >> 3) & 7, paper = 7 - ink;
        for (int y = 0; y < SCR_H; y++)
            for (int X = 0; X < HIRES_W; X++)
            {
                const byte *bank = (X & 1) ? b1 : b0; int col = X >> 1;
                int on = (bank[PIX_OFF(y, col)] >> (7 - (col & 7))) & 1;
                out[y * HIRES_W + X] = (byte)((on ? ink : paper) | 8);   // bright
            }
        return out;
    }

    // Standard (8x8 attrs) and hi-colour (8x1 attrs) share this path; they
    // differ only in where the attribute byte lives.
    int isHi = (len == TMX_SIZE || len == TMX_SIZE + PAL_SIZE);
    *w = SCR_W; *h = SCR_H;
    byte *out = (byte *)malloc((size_t)SCR_W * SCR_H);
    if (!out) die("Memory allocation error", NULL);
    const byte *bmp = scr;
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++)
        {
            int o = PIX_OFF(y, x);
            int on = (bmp[o] >> (7 - (x & 7))) & 1;
            byte attr = isHi ? scr[SCR_PIXELS + o] : scr[SCR_PIXELS + ATTR_AT(y, x)];
            if (clut)
            {
                // ULAplus: FLASH/BRIGHT are group bits, no animation; the index
                // IS the 0-63 CLUT slot.
                int grp = (attr >> 6) & 3;
                out[y * SCR_W + x] = (byte)(on ? (grp * 16 + (attr & 7))
                                              : (grp * 16 + 8 + ((attr >> 3) & 7)));
            }
            else
            {
                int flash = attr & 0x80;
                if (flash) *hasFlash = 1;
                // In a flashing cell on the swapped frame, ink and paper trade.
                int useInk = on;
                if (flash && swap) useInk = !on;
                int colour = useInk ? (attr & 7) : ((attr >> 3) & 7);
                out[y * SCR_W + x] = (byte)(colour | ((attr & 0x40) ? 8 : 0));
            }
        }
    return out;
}

// Upscale a logical index buffer by an integer factor into a new buffer.
static byte *scaleIndices(const byte *in, int w, int h, int scale, int *ow, int *oh)
{
    *ow = w * scale; *oh = h * scale;
    byte *out = (byte *)malloc((size_t)*ow * *oh);
    if (!out) die("Memory allocation error", NULL);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            byte v = in[y * w + x];
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    out[(size_t)(y * scale + sy) * *ow + (x * scale + sx)] = v;
        }
    return out;
}

static void cmdScr2Gif(const char *in, const char *out, int normal, int scale, int delayCs)
{
    size_t len;
    byte *scr = readFile(in, &len);
    Detected d = detect(len);
    if (!d.valid) { free(scr); die(BAD_SIZE_MSG, in); }

    // ULAplus attribute screens carry their own 64-colour palette and never
    // flash; hi-res keeps its 16-entry fixed palette even when 64 regs trail.
    const byte *clut = (d.palette && d.mode != mHiRes) ? scr + len - PAL_SIZE : NULL;

    // Build the two candidate frames; keep only the second if it differs.
    int w, h, flashA, flashB, ow, oh;
    byte *fa = sceneIndices(scr, len, 0, clut, &w, &h, &flashA);
    byte *fb = sceneIndices(scr, len, 1, clut, &w, &h, &flashB);
    int animate = flashA;                       // FLASH present -> two frames
    byte *frame0 = scaleIndices(fa, w, h, scale, &ow, &oh);
    byte *frame1 = animate ? scaleIndices(fb, w, h, scale, &ow, &oh) : NULL;
    free(fa); free(fb);

    // Global colour table: 64 GRB332 entries for ULAplus, else the 16 fixed
    // Spectrum colours (index = colour | bright<<3).
    int palBits = clut ? 6 : 4, palN = 1 << palBits;
    byte pal[64 * 3];
    if (clut) for (int i = 0; i < 64; i++) plusToRGB(clut[i], &pal[i*3], &pal[i*3+1], &pal[i*3+2]);
    else      for (int i = 0; i < 16; i++) indexToRGB(i & 7, i & 8, normal, &pal[i*3], &pal[i*3+1], &pal[i*3+2]);

    Buf g = {0};
    bufMem(&g, "GIF89a", 6);
    bufLE16(&g, ow); bufLE16(&g, oh);
    bufByte(&g, 0x80 | ((palBits - 1) << 4) | (palBits - 1));   // GCT, palN entries
    bufByte(&g, 0);                     // background colour index
    bufByte(&g, 0);                     // pixel aspect ratio
    bufMem(&g, pal, (size_t)palN * 3);

    if (animate)
    {
        // NETSCAPE2.0 application extension -> loop forever.
        bufByte(&g, 0x21); bufByte(&g, 0xFF); bufByte(&g, 0x0B);
        bufMem(&g, "NETSCAPE2.0", 11);
        bufByte(&g, 0x03); bufByte(&g, 0x01); bufLE16(&g, 0); bufByte(&g, 0x00);
    }

    int nframes = animate ? 2 : 1;
    const byte *frames[2] = { frame0, frame1 };
    for (int fr = 0; fr < nframes; fr++)
    {
        if (animate)
        {
            // Graphic Control Extension: per-frame delay, no transparency.
            bufByte(&g, 0x21); bufByte(&g, 0xF9); bufByte(&g, 0x04);
            bufByte(&g, 0x00);              // disposal 0, no transparent index
            bufLE16(&g, delayCs);           // delay in centiseconds
            bufByte(&g, 0x00);              // transparent colour index (unused)
            bufByte(&g, 0x00);              // block terminator
        }
        bufByte(&g, 0x2C);                  // Image Descriptor
        bufLE16(&g, 0); bufLE16(&g, 0);     // left, top
        bufLE16(&g, ow); bufLE16(&g, oh);   // width, height
        bufByte(&g, 0x00);                  // no local colour table
        gifImageData(&g, frames[fr], (size_t)ow * oh, palBits);
    }
    bufByte(&g, 0x3B);                       // trailer

    writeFile(out, g.p, g.n);
    free(g.p); free(frame0); free(frame1); free(scr);
    printf("Wrote %s (%dx%d, %s)\n", out, ow, oh,
           animate ? "2-frame FLASH animation" : "static, no FLASH cells");
}

//~~~~ info ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

static const char *kColour[8] =
    { "black", "blue", "red", "magenta", "green", "cyan", "yellow", "white" };

// Dump a 64-entry CLUT as 4 groups x (8 ink + 8 paper) GRB332 -> RGB hex.
static void printPalette(const byte *clut)
{
    printf("ULAplus palette (64 GRB332 registers):\n");
    for (int grp = 0; grp < 4; grp++)
    {
        printf("  group %d  ink:", grp);
        for (int s = 0; s < 8; s++)
        {
            byte r, g, b; plusToRGB(clut[grp * 16 + s], &r, &g, &b);
            printf(" %02X%02X%02X", r, g, b);
        }
        printf("\n           paper:");
        for (int s = 0; s < 8; s++)
        {
            byte r, g, b; plusToRGB(clut[grp * 16 + 8 + s], &r, &g, &b);
            printf(" %02X%02X%02X", r, g, b);
        }
        printf("\n");
    }
}

static void cmdInfo(const char *in)
{
    size_t len;
    byte *scr = readFile(in, &len);
    Detected d = detect(len);
    printf("File: %s\n", in);
    printf("Size: %zu bytes\n", len);
    if (!d.valid) { printf("Mode: unrecognised\n"); free(scr); return; }

    const byte *clut = d.palette ? scr + len - PAL_SIZE : NULL;

    if (d.mode == mStd || d.mode == mHiColour)
    {
        int isHi = (d.mode == mHiColour);
        printf("Mode: %s%s\n",
               isHi ? "Timex hi-colour (256x192, 8x1 attrs)"
                    : "standard (256x192, 8x8 attrs)",
               d.palette ? " + ULAplus 64-colour palette" : "");
        const byte *att = scr + SCR_PIXELS;
        int n = isHi ? SCR_PIXELS : SCR_ATTRS;
        if (d.palette)
        {
            // FLASH/BRIGHT are CLUT group bits here, not flash/bright.
            int grpUse[4] = {0};
            for (int i = 0; i < n; i++) grpUse[(att[i] >> 6) & 3]++;
            printf("CLUT group use:");
            for (int gpe = 0; gpe < 4; gpe++) printf(" %d:%d", gpe, grpUse[gpe]);
            printf("\n");
            printPalette(clut);
        }
        else
        {
            int inkUse[8] = {0}, paperUse[8] = {0}, bright = 0, flash = 0;
            for (int i = 0; i < n; i++)
            {
                byte a = att[i];
                inkUse[a & 7]++; paperUse[(a >> 3) & 7]++;
                if (a & 0x40) bright++;
                if (a & 0x80) flash++;
            }
            printf("Bright attrs: %d / %d\n", bright, n);
            printf("Flash attrs:  %d / %d\n", flash, n);
            printf("Ink:   "); for (int c = 0; c < 8; c++) if (inkUse[c]) printf("%s(%d) ", kColour[c], inkUse[c]);
            printf("\nPaper: "); for (int c = 0; c < 8; c++) if (paperUse[c]) printf("%s(%d) ", kColour[c], paperUse[c]);
            printf("\n");
        }
    }
    else  // hi-res
    {
        int port = scr[TMX_SIZE], ink = (port >> 3) & 7;
        printf("Mode: Timex hi-res (512x192 mono)%s\n",
               d.palette ? " + ULAplus palette (not applied to render)" : "");
        printf("Colour byte: 0x%02X -> ink %s on paper %s (bright)\n",
               port, kColour[ink], kColour[7 - ink]);
        if (d.palette) printPalette(clut);
    }
    free(scr);
}

//~~~~ main ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

int main(int argc, char **argv)
{
    enum Command cmd = cmNone;
    enum Mode mode = mStd;
    const char *fileIn = NULL, *fileOut = NULL;
    int normal = DEF_NORMAL, scale = 1, delayCs = 32, ulaplus = 0, dither = 0;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { printf("%s", Help); return 0; }
        else if (strcmp(a, "--level") == 0 && i + 1 < argc) normal = atoi(argv[++i]) & 0xff;
        else if (strcmp(a, "--scale") == 0 && i + 1 < argc) { scale = atoi(argv[++i]); if (scale < 1) scale = 1; }
        else if (strcmp(a, "--delay") == 0 && i + 1 < argc) { delayCs = atoi(argv[++i]); if (delayCs < 1) delayCs = 1; }
        else if (strcmp(a, "--ulaplus") == 0) ulaplus = 1;
        else if (strcmp(a, "--dither") == 0) dither = 1;
        else if (strcmp(a, "--palette") == 0 && i + 1 < argc)
        {
            const char *p = argv[++i];
            if      (strcmp(p, "standard") == 0) normal = 0xD7;
            else if (strcmp(p, "classic")  == 0) normal = 0xCD;
            else if (strcmp(p, "emulator") == 0) normal = 0xC0;
            else die("Unknown palette (use standard|classic|emulator):", p);
        }
        else if (strcmp(a, "--mode") == 0 && i + 1 < argc)
        {
            const char *m = argv[++i];
            if      (strcmp(m, "std")      == 0) mode = mStd;
            else if (strcmp(m, "hicolour") == 0 || strcmp(m, "hicolor") == 0) mode = mHiColour;
            else if (strcmp(m, "hires")    == 0) mode = mHiRes;
            else die("Unknown mode (use std|hicolour|hires):", m);
        }
        else if (cmd == cmNone && strcmp(a, "png2scr") == 0) cmd = cmPng2Scr;
        else if (cmd == cmNone && strcmp(a, "scr2png") == 0) cmd = cmScr2Png;
        else if (cmd == cmNone && strcmp(a, "scr2gif") == 0) cmd = cmScr2Gif;
        else if (cmd == cmNone && strcmp(a, "info") == 0)    cmd = cmInfo;
        else if (!fileIn)  fileIn = a;
        else if (!fileOut) fileOut = a;
        else die("Unexpected argument:", a);
    }

    if (cmd == cmNone) { fprintf(stderr, "%s", Help); return 1; }
    if (!fileIn) die("Missing input file", NULL);

    switch (cmd)
    {
        case cmPng2Scr:
            if (!fileOut) die("Missing output .scr file", NULL);
            if (ulaplus)
            {
                if      (mode == mStd)      cmdPng2ScrUlaStd(fileIn, fileOut, dither);
                else if (mode == mHiColour) cmdPng2ScrUlaHiColour(fileIn, fileOut, dither);
                else die("hi-res + ULAplus output is not supported "
                         "(the palette mapping is under-specified)", NULL);
            }
            else if (mode == mStd)      cmdPng2ScrStd(fileIn, fileOut, dither);
            else if (mode == mHiColour) cmdPng2ScrHiColour(fileIn, fileOut, dither);
            else                        cmdPng2ScrHiRes(fileIn, fileOut, dither);
            break;
        case cmScr2Png:
            if (!fileOut) die("Missing output .png file", NULL);
            cmdScr2Png(fileIn, fileOut, normal, scale);
            break;
        case cmScr2Gif:
            if (!fileOut) die("Missing output .gif file", NULL);
            cmdScr2Gif(fileIn, fileOut, normal, scale, delayCs);
            break;
        case cmInfo:
            cmdInfo(fileIn);
            break;
        default: break;
    }
    return 0;
}
