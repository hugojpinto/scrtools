// scr-tools - ZX Spectrum SCREEN$ (.scr) manipulation utility
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

// ATTR_AT: byte index (0..767) of the attribute cell covering pixel (x,y).
//   Cells are 8x8, laid out left-to-right, top-to-bottom: 32 per row, 24 rows.
//   (y>>3) = cell row, (x>>3) = cell column.
#define ATTR_AT(y, x) ((y >> 3) * 32 + (x >> 3))

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
#define PIX_OFF(y, x) ((x >> 3) + (y & 7) * 256 + ((y >> 3) & 7) * 32 + (y >> 6) * 2048)

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
    "SCR-TOOLS " STR(BUILD_TS) " - ZX Spectrum SCREEN$ utility\n"
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
    "    --level <n>      Normal (non-bright) colour level for rendering,\n"
    "                     0-255 (default 215 = 0xD7). Bright stays 255.\n"
    "    --palette <p>    Named level preset: standard (215, default),\n"
    "                     classic (205), emulator (192).\n"
    "    --scale <n>      Upscale scr2png / scr2gif output by factor n.\n"
    "    --delay <cs>     FLASH frame delay for scr2gif, centiseconds\n"
    "                     (default 32 = the hardware ~1.56 Hz rate).\n"
    "\n"
    "Examples:\n"
    "    scrtools png2scr factory.png factory.scr\n"
    "    scrtools png2scr --mode hires logo512.png logo.scr\n"
    "    scrtools png2scr --mode hicolour pic.png pic.scr\n"
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

// Hamming distance between two 3-bit colour codes (used to snap a stray pixel
// to whichever of a cell's two chosen colours it is nearest).
static int colourDist(int a, int b) { return __builtin_popcount(a ^ b); }

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

    // Pick the source width/height from the detected mode, allocate the output.
    int srcW = (len == TMX_HIRES) ? HIRES_W : SCR_W;
    int W = srcW * scale, H = SCR_H * scale;
    byte *rgb = (byte *)malloc((size_t)W * H * 3);
    if (!rgb) die("Memory allocation error", NULL);

    if (len == SCR_SIZE)
    {
        // -- Standard: per-pixel bit + per-8x8-cell attribute. --
        for (int y = 0; y < SCR_H; y++)
            for (int x = 0; x < SCR_W; x++)
            {
                // 8 pixels per byte, MSB-first: column x uses bit (7 - x%8).
                int on = (scr[PIX_OFF(y, x)] >> (7 - (x & 7))) & 1;
                byte attr = scr[SCR_PIXELS + ATTR_AT(y, x)];
                int colour = on ? (attr & 7) : ((attr >> 3) & 7);   // INK : PAPER
                byte r, g, b;
                indexToRGB(colour, attr & 0x40, normal, &r, &g, &b);
                putPixel(rgb, W, x, y, scale, r, g, b);
            }
    }
    else if (len == TMX_SIZE)
    {
        // -- Timex hi-colour: bitmap in block 1, 8x1 attributes in block 2 at
        //    the SAME interleaved offset, so attr(x,y) == block2[PIX_OFF]. --
        const byte *bmp = scr, *att = scr + SCR_PIXELS;
        for (int y = 0; y < SCR_H; y++)
            for (int x = 0; x < SCR_W; x++)
            {
                int o = PIX_OFF(y, x);
                int on = (bmp[o] >> (7 - (x & 7))) & 1;
                byte attr = att[o];
                int colour = on ? (attr & 7) : ((attr >> 3) & 7);
                byte r, g, b;
                indexToRGB(colour, attr & 0x40, normal, &r, &g, &b);
                putPixel(rgb, W, x, y, scale, r, g, b);
            }
    }
    else if (len == TMX_HIRES || len == TMX_SIZE + 0 /* never */)
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
    else
    {
        free(rgb); free(scr);
        die("Unrecognised .scr size (need 6912, 12288 or 12289):", in);
    }

    writeRgbPng(out, rgb, W, H);
    free(rgb);
    free(scr);
    printf("Wrote %s (%dx%d)\n", out, W, H);
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

//~~~~ png2scr: standard (8x8 attributes) ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

static void cmdPng2ScrStd(const char *in, const char *out)
{
    int w, h, f;
    byte *px = loadPngRGB(in, SCR_W, &w, &h, &f);
    byte scr[SCR_SIZE];
    memset(scr, 0, sizeof scr);

    // Resolve one INK / PAPER / BRIGHT per 8x8 cell (see the cell algorithm
    // notes inline below). A faithful Spectrum render is lossless here.
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

            scr[SCR_PIXELS + cy * 32 + cx] =
                (byte)(((bright ? 1 : 0) << 6) | (paper << 3) | ink);

            // Emit the bitmap: a pixel is INK when nearer the ink colour.
            for (int py = 0; py < 8; py++)
            {
                int y = cy * 8 + py, bits = 0;
                for (int pxl = 0; pxl < 8; pxl++)
                {
                    const byte *p = samplePx(px, w, f, cx * 8 + pxl, y);
                    int br, idx = classify(p[0], p[1], p[2], &br);
                    if (colourDist(idx, ink) < colourDist(idx, paper))
                        bits |= (0x80 >> pxl);   // leftmost pixel = high bit
                }
                scr[PIX_OFF(y, cx * 8)] = (byte)bits;
            }
        }

    stbi_image_free(px);
    writeFile(out, scr, SCR_SIZE);
    printf("Wrote %s (%d bytes, standard)\n", out, SCR_SIZE);
}

//~~~~ png2scr: Timex hi-colour (8x1 attributes) ~~~~~~~~~~~~~~~~~~~~~~~~~~~//

static void cmdPng2ScrHiColour(const char *in, const char *out)
{
    int w, h, f;
    byte *px = loadPngRGB(in, SCR_W, &w, &h, &f);
    byte *scr = (byte *)calloc(TMX_SIZE, 1);
    if (!scr) die("Memory allocation error", NULL);
    byte *bmp = scr, *att = scr + SCR_PIXELS;

    // Same idea as standard, but a "cell" is now a single 8x1 strip: one
    // attribute per scanline per 8-pixel column. Bitmap byte and attribute byte
    // share the interleaved offset PIX_OFF(y, cx*8).
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

            int o = PIX_OFF(y, cx * 8);
            att[o] = (byte)(((bright ? 1 : 0) << 6) | (paper << 3) | ink);

            int bits = 0;
            for (int pxl = 0; pxl < 8; pxl++)
            {
                const byte *p = samplePx(px, w, f, cx * 8 + pxl, y);
                int br, idx = classify(p[0], p[1], p[2], &br);
                if (colourDist(idx, ink) < colourDist(idx, paper))
                    bits |= (0x80 >> pxl);
            }
            bmp[o] = (byte)bits;
        }

    stbi_image_free(px);
    writeFile(out, scr, TMX_SIZE);
    free(scr);
    printf("Wrote %s (%d bytes, Timex hi-colour)\n", out, TMX_SIZE);
}

//~~~~ png2scr: Timex hi-res (512x192 mono + colour byte) ~~~~~~~~~~~~~~~~~~//

static void cmdPng2ScrHiRes(const char *in, const char *out)
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

    // Weave the 512-wide image into the two interleaved 256-wide banks:
    // even output columns -> bank 0, odd -> bank 1. A pixel is INK when nearer
    // the ink colour than the paper colour.
    for (int y = 0; y < SCR_H; y++)
        for (int X = 0; X < HIRES_W; X++)
        {
            const byte *p = samplePx(px, w, f, X, y);
            int br, idx = classify(p[0], p[1], p[2], &br);
            if (colourDist(idx, ink) < colourDist(idx, paper))
            {
                byte *bank = (X & 1) ? bank1 : bank0;
                int col = X >> 1;
                bank[PIX_OFF(y, col)] |= (0x80 >> (col & 7));
            }
        }
    // Trailing colour byte = raw port-0xFF value: mode 0x06 (hi-res) in bits
    // 0-2, INK in bits 3-5. Emulators store and re-read exactly this byte.
    scr[TMX_SIZE] = (byte)(0x06 | (ink << 3));

    stbi_image_free(px);
    writeFile(out, scr, TMX_HIRES);
    free(scr);
    printf("Wrote %s (%d bytes, Timex hi-res, ink=%d paper=%d)\n",
           out, TMX_HIRES, ink, paper);
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
static byte *sceneIndices(const byte *scr, size_t len, int swap,
                          int *w, int *h, int *hasFlash)
{
    *hasFlash = 0;
    if (len == TMX_HIRES)
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
    int isHi = (len == TMX_SIZE);
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
            int flash = attr & 0x80;
            if (flash) *hasFlash = 1;
            // In a flashing cell on the swapped frame, ink and paper trade.
            int useInk = on;
            if (flash && swap) useInk = !on;
            int colour = useInk ? (attr & 7) : ((attr >> 3) & 7);
            out[y * SCR_W + x] = (byte)(colour | ((attr & 0x40) ? 8 : 0));
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
    if (len != SCR_SIZE && len != TMX_SIZE && len != TMX_HIRES)
    {
        free(scr);
        die("Unrecognised .scr size (need 6912, 12288 or 12289):", in);
    }

    // Build the two candidate frames; keep only the second if it differs.
    int w, h, flashA, flashB, ow, oh;
    byte *fa = sceneIndices(scr, len, 0, &w, &h, &flashA);
    byte *fb = sceneIndices(scr, len, 1, &w, &h, &flashB);
    int animate = flashA;                       // FLASH present -> two frames
    byte *frame0 = scaleIndices(fa, w, h, scale, &ow, &oh);
    byte *frame1 = animate ? scaleIndices(fb, w, h, scale, &ow, &oh) : NULL;
    free(fa); free(fb);

    // 16-entry global colour table: index = colour | bright<<3.
    byte pal[16 * 3];
    for (int i = 0; i < 16; i++)
        indexToRGB(i & 7, i & 8, normal, &pal[i * 3], &pal[i * 3 + 1], &pal[i * 3 + 2]);

    Buf g = {0};
    bufMem(&g, "GIF89a", 6);
    bufLE16(&g, ow); bufLE16(&g, oh);
    bufByte(&g, 0x80 | (3 << 4) | 3);   // GCT present, 4-bit colour, 16 entries
    bufByte(&g, 0);                     // background colour index
    bufByte(&g, 0);                     // pixel aspect ratio
    bufMem(&g, pal, sizeof pal);

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
        gifImageData(&g, frames[fr], (size_t)ow * oh, 4);
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

static void cmdInfo(const char *in)
{
    size_t len;
    byte *scr = readFile(in, &len);
    printf("File: %s\n", in);
    printf("Size: %zu bytes\n", len);

    if (len == SCR_SIZE || len == TMX_SIZE)
    {
        // Both standard and hi-colour carry a block of standard attribute
        // bytes; only the count and geometry differ.
        int isHi = (len == TMX_SIZE);
        printf("Mode: %s\n", isHi ? "Timex hi-colour (256x192, 8x1 attrs)"
                                  : "standard (256x192, 8x8 attrs)");
        const byte *att = scr + SCR_PIXELS;
        int n = isHi ? SCR_PIXELS : SCR_ATTRS;
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
    else if (len == TMX_HIRES)
    {
        int port = scr[TMX_SIZE], ink = (port >> 3) & 7;
        printf("Mode: Timex hi-res (512x192 mono)\n");
        printf("Colour byte: 0x%02X -> ink %s on paper %s (bright)\n",
               port, kColour[ink], kColour[7 - ink]);
    }
    else
    {
        printf("Mode: unrecognised (expected 6912, 12288 or 12289)\n");
    }
    free(scr);
}

//~~~~ main ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

int main(int argc, char **argv)
{
    enum Command cmd = cmNone;
    enum Mode mode = mStd;
    const char *fileIn = NULL, *fileOut = NULL;
    int normal = DEF_NORMAL, scale = 1, delayCs = 32;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { printf("%s", Help); return 0; }
        else if (strcmp(a, "--level") == 0 && i + 1 < argc) normal = atoi(argv[++i]) & 0xff;
        else if (strcmp(a, "--scale") == 0 && i + 1 < argc) { scale = atoi(argv[++i]); if (scale < 1) scale = 1; }
        else if (strcmp(a, "--delay") == 0 && i + 1 < argc) { delayCs = atoi(argv[++i]); if (delayCs < 1) delayCs = 1; }
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
            if      (mode == mStd)      cmdPng2ScrStd(fileIn, fileOut);
            else if (mode == mHiColour) cmdPng2ScrHiColour(fileIn, fileOut);
            else                        cmdPng2ScrHiRes(fileIn, fileOut);
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
