/*
 * yuv420_hflip_test.c -- Regression test for planar-YUV -> RGB camera
 * preview conversion with a horizontal pre-flip flag.
 *
 * Scenario
 * --------
 * A camera preview pipeline converts planar YUV 4:2:0 to packed RGB.
 * Two defects were observed in the field:
 *
 *   1. The right-hand sampling column occasionally picked up the left
 *      column's chroma (a column-indexing bug at the right edge / half
 *      boundary.)
 *   2. With the pre-flip flag set, the mirrored picture showed color
 *      misalignment: the luma plane was mirrored but the chroma pairing
 *      did not travel with it.
 *
 * Fixed sample "JT-E"
 * -------------------
 *   Y plane:   320 x 240, one byte per pixel
 *   U/V planes: 160 x 120 (4:2:0 -- one chroma sample per 2x2 luma block)
 *
 *   The left and right halves carry distinct, constant chroma so that any
 *   column mix-up is directly visible.  The rightmost luma column is
 *   pinned to Y=235 and the rightmost chroma column to U=V=128, so every
 *   right-edge pixel must decode to pure white (255,255,255) under the
 *   BT.601 studio-swing (video-range) conversion used by the preview
 *   pipeline: 298*(235-16)/256 = 255.
 *
 * Procedure
 * ---------
 * The same fixed planar buffer is decoded twice: once without flipping
 * and once with the pre-flip (horizontal mirror) flag.  The second MCU
 * row (rows 16..31 -- one MCU row spans 16 lines in 4:2:0) is compared
 * pixel by pixel: the flipped output must be the exact horizontal mirror
 * of the unflipped output, i.e. the luma is mirrored and each 2-pixel
 * chroma pair travels with its luma pair instead of being swapped.
 *
 * Exit status 0 = pass, 1 = fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W   320                 /* luma width                      */
#define H   240                 /* luma height                     */
#define CW  (W / 2)             /* chroma width  (160)             */
#define CH  (H / 2)             /* chroma height (120)             */
#define MCU_H 16                /* MCU row height for 4:2:0        */
#define MCU2_Y0 MCU_H           /* second MCU row: rows 16..31     */
#define MCU2_Y1 (2 * MCU_H - 1)

/* Fixed planar sample buffer ("JT-E") */
static unsigned char yplane[H][W];
static unsigned char uplane[CH][CW];
static unsigned char vplane[CH][CW];

/* Decoded RGB24 frames: [0] = no flip, [1] = pre-flip */
static unsigned char frame[2][H][W][3];

static int clip255(int v)
{
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* BT.601 studio-swing (video-range) YUV -> RGB, as used by camera
   preview pipelines.  Y in [16,235] maps to [0,255]. */
static void yuv2rgb(int y, int u, int v, unsigned char *out)
{
  int c = y - 16, d = u - 128, e = v - 128;

  out[0] = (unsigned char)clip255((298 * c + 409 * e + 128) >> 8);
  out[1] = (unsigned char)clip255((298 * c - 100 * d - 208 * e + 128) >> 8);
  out[2] = (unsigned char)clip255((298 * c + 516 * d + 128) >> 8);
}

/*
 * Decode one planar YUV 4:2:0 frame to RGB24.
 *
 * When hflip (the pre-flip flag) is set, output column x samples luma
 * column W-1-x, and -- critically -- the chroma column is derived from
 * the *mirrored* luma column, so each chroma sample stays paired with
 * the same 2-pixel luma group it belonged to before the flip.
 */
static void decode_frame(int hflip, unsigned char dst[H][W][3])
{
  int x, y;

  for (y = 0; y < H; y++) {
    for (x = 0; x < W; x++) {
      int sx = hflip ? W - 1 - x : x;   /* mirrored luma column       */
      int scx = sx >> 1;                /* chroma pair follows luma   */

      yuv2rgb(yplane[y][sx], uplane[y >> 1][scx], vplane[y >> 1][scx],
              dst[y][x]);
    }
  }
}

/* Build the fixed "JT-E" sample. */
static void build_sample_jte(void)
{
  int x, y;

  /* Luma: deterministic horizontal ramp (video range 16..235) with a
     per-row parity wiggle so row indexing is also exercised; the right
     edge column is pinned to Y=235. */
  for (y = 0; y < H; y++) {
    for (x = 0; x < W; x++) {
      int v = 16 + ((x + (y & 1)) * 218) / (W - 1);

      if (x == W - 1)
        v = 235;
      if (v > 235)
        v = 235;
      yplane[y][x] = (unsigned char)v;
    }
  }

  /* Chroma: distinct constant chroma in each half so a column mix-up
     changes the decoded color; the rightmost chroma column is neutral
     (U=V=128) so the right edge decodes to pure white. */
  for (y = 0; y < CH; y++) {
    for (x = 0; x < CW; x++) {
      if (x < CW / 2) {               /* left half:  U=90,  V=160 */
        uplane[y][x] = 90;
        vplane[y][x] = 160;
      } else if (x < CW - 1) {        /* right half: U=170, V=96  */
        uplane[y][x] = 170;
        vplane[y][x] = 96;
      } else {                        /* right edge: neutral      */
        uplane[y][x] = 128;
        vplane[y][x] = 128;
      }
    }
  }
}

static int failures = 0;

static void expect_pixel(const char *what, int run, int y, int x,
                         unsigned char r, unsigned char g, unsigned char b)
{
  unsigned char *p = frame[run][y][x];

  if (p[0] != r || p[1] != g || p[2] != b) {
    printf("FAIL: %s: run %d pixel (%d,%d) = (%u,%u,%u), "
           "expected (%u,%u,%u)\n", what, run, x, y, p[0], p[1], p[2],
           r, g, b);
    failures++;
  }
}

int main(void)
{
  int x, y;

  build_sample_jte();

  /* Decode the same fixed planar buffer twice: no flip, then pre-flip. */
  decode_frame(0, frame[0]);
  decode_frame(1, frame[1]);

  /* 1. Second MCU row (rows 16..31), pixel by pixel: the flipped frame
     must be the exact horizontal mirror of the unflipped frame -- luma
     mirrored, chroma pairing not swapped. */
  for (y = MCU2_Y0; y <= MCU2_Y1; y++) {
    for (x = 0; x < W; x++) {
      unsigned char *a = frame[0][y][W - 1 - x];
      unsigned char *b = frame[1][y][x];

      if (memcmp(a, b, 3) != 0) {
        printf("FAIL: MCU row 2 mirror mismatch at (%d,%d): "
               "flipped (%u,%u,%u) != unflipped mirror (%u,%u,%u)\n",
               x, y, b[0], b[1], b[2], a[0], a[1], a[2]);
        failures++;
      }
    }
  }

  /* 2. Same mirror invariant over the rest of the frame. */
  for (y = 0; y < H; y++) {
    if (y >= MCU2_Y0 && y <= MCU2_Y1)
      continue;                       /* already checked above */
    for (x = 0; x < W; x++) {
      unsigned char *a = frame[0][y][W - 1 - x];
      unsigned char *b = frame[1][y][x];

      if (memcmp(a, b, 3) != 0) {
        printf("FAIL: mirror mismatch at (%d,%d): "
               "flipped (%u,%u,%u) != unflipped mirror (%u,%u,%u)\n",
               x, y, b[0], b[1], b[2], a[0], a[1], a[2]);
        failures++;
      }
    }
  }

  /* 3. Edge pixel: Y=235/U=128/V=128 must decode to (255,255,255) --
     on the right edge without flip, on the left edge with flip. */
  for (y = MCU2_Y0; y <= MCU2_Y1; y++) {
    expect_pixel("right-edge white", 0, y, W - 1, 255, 255, 255);
    expect_pixel("left-edge white (flipped)", 1, y, 0, 255, 255, 255);
  }

  /* 4. Left/right halves keep their corresponding chroma (guards
     against the right sampling column picking up the left column).
     Reference values computed from the JT-E planes via the BT.601
     studio-swing formula. */
  expect_pixel("left-half chroma", 0, 16, 100, 130, 68, 3);
  expect_pixel("right-half chroma", 0, 16, 200, 107, 168, 243);
  expect_pixel("right-half first column", 0, 16, 160, 76, 136, 212);
  /* ... and the pairing survives the flip at the mirrored positions. */
  expect_pixel("left-half chroma (flipped)", 1, 16, W - 1 - 100,
               130, 68, 3);
  expect_pixel("right-half chroma (flipped)", 1, 16, W - 1 - 200,
               107, 168, 243);
  expect_pixel("right-half first column (flipped)", 1, 16, W - 1 - 160,
               76, 136, 212);

  if (failures) {
    printf("yuv420_hflip_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("yuv420_hflip_test: OK (JT-E 320x240 4:2:0, 2 runs, "
         "second MCU row verified pixel by pixel)\n");
  return 0;
}
