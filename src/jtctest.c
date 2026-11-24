/*
 * Copyright (C) 2026 D. R. Commander
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the libjpeg-turbo Project nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS",
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * JT-C: Lossless JPEG re-encode request sequence
 *
 * This program simulates an archival system that interleaves lossy and
 * lossless JPEG writes through the same TurboJPEG instance.  Using a fixed
 * 17x9 grayscale sample, it issues four requests against a single
 * compression handle:
 *
 *   1. Probe:         Confirm that the handle can losslessly re-encode the
 *                     sample (the request returns "lossless-capable").
 *   2. Success:       Losslessly re-encode the sample into the target file.
 *                     The round-tripped pixels must match point-for-point,
 *                     and the lossless header fields must be retained.
 *   3. Error options: Mixing a DCT option (TJPARAM_FASTDCT) with lossless
 *                     compression must be rejected before any data is
 *                     written.  The target file must remain empty, and the
 *                     handle must remain usable.
 *   4. Success again: After the DCT option is cleared, lossless re-encoding
 *                     succeeds again.
 *
 * The lossy path is exercised as well, to confirm that each path retains
 * its own header fields.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "turbojpeg.h"

#define WIDTH  17
#define HEIGHT 9

static int exitStatus = EXIT_SUCCESS;

#define CHECK(condition, ...) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL: "); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    exitStatus = EXIT_FAILURE; \
    goto bailout; \
  } \
} while (0)


/* Overwrite the target file, or leave it empty if size is 0, emulating an
   archival system that opens the target file before writing. */
static int writeTarget(const char *filename, const unsigned char *buf,
                       size_t size)
{
  FILE *file = fopen(filename, "wb");

  if (!file) return -1;
  if (size && fwrite(buf, 1, size, file) != size) {
    fclose(file);
    return -1;
  }
  return fclose(file);
}


static long targetSize(const char *filename)
{
  FILE *file = fopen(filename, "rb");
  long size;

  if (!file) return -1;
  if (fseek(file, 0, SEEK_END) < 0 || (size = ftell(file)) < 0)
    size = -1;
  fclose(file);
  return size;
}


int main(int argc, char **argv)
{
  const char *target = argc > 1 ? argv[1] : "jtc-out.jpg";
  unsigned char srcBuf[WIDTH * HEIGHT], dstBuf[WIDTH * HEIGHT];
  unsigned char *jpegBuf = NULL;
  size_t jpegSize = 0;
  tjhandle chandle = NULL, dhandle = NULL;
  int x, y;

  /* Fixed 17x9 grayscale sample covering the full sample range. */
  for (y = 0; y < HEIGHT; y++)
    for (x = 0; x < WIDTH; x++)
      srcBuf[y * WIDTH + x] = (unsigned char)((x * 29 + y * 47 + x * y) & 0xFF);

  chandle = tj3Init(TJINIT_COMPRESS);
  dhandle = tj3Init(TJINIT_DECOMPRESS);
  CHECK(chandle && dhandle, "tj3Init() failed");

  /***** Request 1: probe *****/
  printf("Request 1 (probe): ");
  CHECK(tj3Set(chandle, TJPARAM_LOSSLESS, 1) == 0 &&
        tj3Set(chandle, TJPARAM_LOSSLESSPSV, 1) == 0 &&
        tj3Set(chandle, TJPARAM_LOSSLESSPT, 0) == 0,
        "tj3Set(): %s", tj3GetErrorStr(chandle));
  CHECK(tj3Compress8(chandle, srcBuf, WIDTH, 0, HEIGHT, TJPF_GRAY, &jpegBuf,
                     &jpegSize) == 0,
        "tj3Compress8(): %s", tj3GetErrorStr(chandle));
  CHECK(tj3DecompressHeader(dhandle, jpegBuf, jpegSize) == 0,
        "tj3DecompressHeader(): %s", tj3GetErrorStr(dhandle));
  CHECK(tj3Get(dhandle, TJPARAM_LOSSLESS) == 1,
        "handle is not lossless-capable");
  CHECK(tj3Get(dhandle, TJPARAM_JPEGWIDTH) == WIDTH &&
        tj3Get(dhandle, TJPARAM_JPEGHEIGHT) == HEIGHT,
        "incorrect JPEG dimensions %dx%d",
        tj3Get(dhandle, TJPARAM_JPEGWIDTH),
        tj3Get(dhandle, TJPARAM_JPEGHEIGHT));
  printf("lossless-capable\n");
  tj3Free(jpegBuf);  jpegBuf = NULL;  jpegSize = 0;

  /***** Request 2: success *****/
  printf("Request 2 (lossless re-encode): ");
  CHECK(tj3Compress8(chandle, srcBuf, WIDTH, 0, HEIGHT, TJPF_GRAY, &jpegBuf,
                     &jpegSize) == 0,
        "tj3Compress8(): %s", tj3GetErrorStr(chandle));
  CHECK(writeTarget(target, jpegBuf, jpegSize) == 0,
        "cannot write target file %s", target);
  /* The lossless path retains its own header fields. */
  CHECK(tj3DecompressHeader(dhandle, jpegBuf, jpegSize) == 0,
        "tj3DecompressHeader(): %s", tj3GetErrorStr(dhandle));
  CHECK(tj3Get(dhandle, TJPARAM_LOSSLESS) == 1 &&
        tj3Get(dhandle, TJPARAM_LOSSLESSPSV) == 1 &&
        tj3Get(dhandle, TJPARAM_LOSSLESSPT) == 0 &&
        tj3Get(dhandle, TJPARAM_PRECISION) == 8,
        "lossless header fields not retained "
        "(LOSSLESS=%d PSV=%d PT=%d PRECISION=%d)",
        tj3Get(dhandle, TJPARAM_LOSSLESS),
        tj3Get(dhandle, TJPARAM_LOSSLESSPSV),
        tj3Get(dhandle, TJPARAM_LOSSLESSPT),
        tj3Get(dhandle, TJPARAM_PRECISION));
  /* The round-tripped pixels must match point-for-point. */
  memset(dstBuf, 0, sizeof(dstBuf));
  CHECK(tj3Decompress8(dhandle, jpegBuf, jpegSize, dstBuf, 0, TJPF_GRAY) == 0,
        "tj3Decompress8(): %s", tj3GetErrorStr(dhandle));
  CHECK(memcmp(srcBuf, dstBuf, sizeof(srcBuf)) == 0,
        "round-tripped pixels differ from source");
  printf("%zu-byte JPEG, pixels identical\n", jpegSize);
  tj3Free(jpegBuf);  jpegBuf = NULL;  jpegSize = 0;

  /***** Request 3: error options *****/
  printf("Request 3 (DCT option + lossless): ");
  /* The DCT option is accepted by tj3Set() but must be rejected before any
     data is written. */
  CHECK(tj3Set(chandle, TJPARAM_FASTDCT, 1) == 0,
        "tj3Set(TJPARAM_FASTDCT): %s", tj3GetErrorStr(chandle));
  /* Emulate the archival system opening the target file for this request. */
  CHECK(writeTarget(target, NULL, 0) == 0,
        "cannot open target file %s", target);
  CHECK(tj3Compress8(chandle, srcBuf, WIDTH, 0, HEIGHT, TJPF_GRAY, &jpegBuf,
                     &jpegSize) == -1,
        "DCT option + lossless was not rejected");
  printf("rejected before writing (%s)\n", tj3GetErrorStr(chandle));
  CHECK(tj3GetErrorCode(chandle) == TJERR_FATAL,
        "unexpected error code %d", tj3GetErrorCode(chandle));
  CHECK(strstr(tj3GetErrorStr(chandle), "TJPARAM_FASTDCT") != NULL,
        "error message does not identify the DCT option: %s",
        tj3GetErrorStr(chandle));
  CHECK(jpegBuf == NULL && jpegSize == 0,
        "data was written despite the rejected request");
  CHECK(targetSize(target) == 0,
        "target file is not empty (%ld bytes)", targetSize(target));

  /***** Request 4: success again (the handle remains usable) *****/
  printf("Request 4 (lossless re-encode, again): ");
  CHECK(tj3Set(chandle, TJPARAM_FASTDCT, 0) == 0,
        "tj3Set(TJPARAM_FASTDCT): %s", tj3GetErrorStr(chandle));
  CHECK(tj3Compress8(chandle, srcBuf, WIDTH, 0, HEIGHT, TJPF_GRAY, &jpegBuf,
                     &jpegSize) == 0,
        "handle is unusable after the rejected request: %s",
        tj3GetErrorStr(chandle));
  CHECK(writeTarget(target, jpegBuf, jpegSize) == 0,
        "cannot write target file %s", target);
  memset(dstBuf, 0, sizeof(dstBuf));
  CHECK(tj3Decompress8(dhandle, jpegBuf, jpegSize, dstBuf, 0, TJPF_GRAY) == 0,
        "tj3Decompress8(): %s", tj3GetErrorStr(dhandle));
  CHECK(memcmp(srcBuf, dstBuf, sizeof(srcBuf)) == 0,
        "round-tripped pixels differ from source");
  printf("%zu-byte JPEG, pixels identical\n", jpegSize);
  tj3Free(jpegBuf);  jpegBuf = NULL;  jpegSize = 0;

  /***** Cross-check: the lossy path retains its own header fields *****/
  printf("Cross-check (lossy path): ");
  CHECK(tj3Set(chandle, TJPARAM_LOSSLESS, 0) == 0 &&
        tj3Set(chandle, TJPARAM_QUALITY, 90) == 0 &&
        tj3Set(chandle, TJPARAM_SUBSAMP, TJSAMP_GRAY) == 0 &&
        tj3Set(chandle, TJPARAM_FASTDCT, 1) == 0,
        "tj3Set(): %s", tj3GetErrorStr(chandle));
  CHECK(tj3Compress8(chandle, srcBuf, WIDTH, 0, HEIGHT, TJPF_GRAY, &jpegBuf,
                     &jpegSize) == 0,
        "DCT option was rejected on the lossy path: %s",
        tj3GetErrorStr(chandle));
  CHECK(tj3DecompressHeader(dhandle, jpegBuf, jpegSize) == 0,
        "tj3DecompressHeader(): %s", tj3GetErrorStr(dhandle));
  CHECK(tj3Get(dhandle, TJPARAM_LOSSLESS) == 0 &&
        tj3Get(dhandle, TJPARAM_SUBSAMP) == TJSAMP_GRAY &&
        tj3Get(dhandle, TJPARAM_PRECISION) == 8,
        "lossy header fields not retained (LOSSLESS=%d SUBSAMP=%d "
        "PRECISION=%d)",
        tj3Get(dhandle, TJPARAM_LOSSLESS),
        tj3Get(dhandle, TJPARAM_SUBSAMP),
        tj3Get(dhandle, TJPARAM_PRECISION));
  printf("lossy header fields retained\n");
  tj3Free(jpegBuf);  jpegBuf = NULL;  jpegSize = 0;

bailout:
  tj3Free(jpegBuf);
  if (chandle) tj3Destroy(chandle);
  if (dhandle) tj3Destroy(dhandle);
  if (exitStatus == EXIT_SUCCESS)
    printf("JT-C: all requests behaved as expected\n");
  return exitStatus;
}
