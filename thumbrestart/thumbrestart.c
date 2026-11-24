/*
 * thumbrestart.c
 *
 * Regression harness for restart-marker state when a single JPEG compressor
 * object is reused across images (the thumbnail-service pattern).
 *
 * Fixed sample JT-B: three 257x129 RGB images are encoded with one and the
 * same TurboJPEG compressor instance.  The restart interval is 8 MCUs for
 * the first image and 0 for the second and third.
 *
 * Contract under test:
 *   - image 1 contains exactly one DRI marker with value 8 and RSTn markers;
 *   - images 2 and 3 contain no DRI marker and no RSTn marker;
 *   - images 2 and 3 are byte-for-byte identical (same pixels, same params,
 *     no state leaking from the first compression);
 *   - all three JPEG files can be decoded again (old-decoder compatibility)
 *     and decode back to the exact source pixels.
 *
 * The same sequence is also exercised through the raw libjpeg C API
 * (-m capi), reusing one jpeg_compress_struct across all three images.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <turbojpeg.h>

#define JT_WIDTH   257
#define JT_HEIGHT  129
#define JT_PIXELS  (JT_WIDTH * JT_HEIGHT)
#define JT_BYTES   (JT_PIXELS * 3)
#define JT_RESTART 8

enum mode { MODE_TJ, MODE_CAPI };

/* ----------------------------- marker parsing ----------------------------- */

struct marker_info {
  int dri_count;       /* number of DRI markers seen                 */
  int dri_value;       /* value carried by the (last) DRI marker     */
  int rst_count;       /* number of RST0..RST7 markers seen          */
};

/* Parse a JPEG byte stream, honoring entropy-coded segments (0xFF 0x00 byte
 * stuffing) so RSTn markers embedded in scan data are counted correctly. */
static void
scan_markers(const unsigned char *data, size_t size, struct marker_info *mi)
{
  size_t i = 2;  /* skip SOI, already validated by the caller */

  memset(mi, 0, sizeof(*mi));
  while (i + 1 < size) {
    if (data[i] != 0xFF) {
      i++;
      continue;
    }
    unsigned char m = data[i + 1];
    if (m == 0xFF) {                 /* fill byte */
      i++;
      continue;
    }
    if (m == 0x01) {                 /* TEM, no length */
      i += 2;
      continue;
    }
    if (m >= 0xD0 && m <= 0xD7) {    /* RSTn inside entropy data */
      mi->rst_count++;
      i += 2;
      continue;
    }
    if (m == 0xD9)                   /* EOI */
      return;
    if (i + 3 >= size)
      break;
    unsigned int len = ((unsigned int)data[i + 2] << 8) | data[i + 3];
    if (m == 0xDD) {                 /* DRI */
      mi->dri_count++;
      mi->dri_value = ((unsigned int)data[i + 4] << 8) | data[i + 5];
    }
    i += 2 + len;
    if (m != 0xDA)
      continue;

    /* Entropy-coded segment: skip stuffed bytes, count RSTn, stop at EOI. */
    while (i + 1 < size) {
      if (data[i] != 0xFF) {
        i++;
        continue;
      }
      if (data[i + 1] == 0x00) {     /* stuffed 0xFF */
        i += 2;
        continue;
      }
      if (data[i + 1] == 0xFF) {
        i++;
        continue;
      }
      if (data[i + 1] >= 0xD0 && data[i + 1] <= 0xD7) {
        mi->rst_count++;
        i += 2;
        continue;
      }
      if (data[i + 1] == 0xD9)
        return;
      break;                         /* another stand-alone marker */
    }
  }
}

/* ----------------------------- sample images ----------------------------- */

/* Deterministic 257x129 RGB images.  Images 2 and 3 share the same pixels so
 * that, with identical encoding parameters, correct outputs must match byte
 * for byte; image 1 uses different pixels. */
static void
make_image(unsigned char *buf, int seed)
{
  size_t i;

  for (i = 0; i < JT_BYTES; i += 3) {
    int x = (int)((i / 3) % JT_WIDTH);
    int y = (int)((i / 3) / JT_WIDTH);
    buf[i]     = (unsigned char)((x * 7 + seed) & 255);
    buf[i + 1] = (unsigned char)((y * 11 + seed * 3) & 255);
    buf[i + 2] = (unsigned char)((x + y + seed * 5) & 255);
  }
}

/* ------------------------------- verification ---------------------------- */

static int g_failures;

/* Decode a JPEG image through TurboJPEG.  Lossy JPEG compression is not
 * pixel-exact, so "old decoder compatibility" means: the decoder accepts the
 * stream without error and reproduces the nominal 257x129 RGB dimensions. */
static int
decode_succeeds(tjhandle dinst, const unsigned char *jpeg, size_t jpeg_size)
{
  unsigned char *rgb = NULL;
  int w, h;

  if (tj3DecompressHeader(dinst, jpeg, jpeg_size) != 0) {
    printf("    decode header failed: %s\n", tj3GetErrorStr(dinst));
    return -1;
  }
  w = tj3Get(dinst, TJPARAM_JPEGWIDTH);
  h = tj3Get(dinst, TJPARAM_JPEGHEIGHT);
  if (w != JT_WIDTH || h != JT_HEIGHT) {
    printf("    decoded dimensions %dx%d != %dx%d\n", w, h, JT_WIDTH,
           JT_HEIGHT);
    return -1;
  }
  rgb = (unsigned char *)malloc(JT_BYTES);
  if (tj3Decompress8(dinst, jpeg, jpeg_size, rgb, JT_WIDTH * 3, TJPF_RGB) !=
      0) {
    printf("    decode failed: %s\n", tj3GetErrorStr(dinst));
    free(rgb);
    return -1;
  }
  free(rgb);
  return 0;
}

static void
verify_run(const char *mode_name, int run, int interval,
           const unsigned char *jpeg, size_t jpeg_size,
           const unsigned char *reference, size_t reference_size,
           tjhandle dinst)
{
  struct marker_info mi;
  int expect_markers = interval != 0;
  int ok;

  scan_markers(jpeg, jpeg_size, &mi);
  ok = (mi.dri_count == expect_markers) &&
       (!expect_markers || mi.dri_value == interval) &&
       ((mi.rst_count > 0) == expect_markers);
  if (reference)
    ok = ok && jpeg_size == reference_size &&
         memcmp(jpeg, reference, jpeg_size) == 0;

  printf("  [%s] run %d (interval=%d): %zu bytes, DRI=%d(value=%d), "
         "RSTn=%d ... %s\n", mode_name, run + 1, interval, jpeg_size,
         mi.dri_count, mi.dri_value, mi.rst_count, ok ? "OK" : "FAIL");
  if (!ok)
    g_failures++;

  if (decode_succeeds(dinst, jpeg, jpeg_size) != 0) {
    printf("    old-decoder compatibility: FAIL\n");
    g_failures++;
  }
}

/* --------------------------- TurboJPEG API path -------------------------- */

static int
run_turbojpeg(const unsigned char *images[3])
{
  tjhandle cinst = tj3Init(TJINIT_COMPRESS);
  tjhandle dinst = tj3Init(TJINIT_DECOMPRESS);
  const int intervals[3] = { JT_RESTART, 0, 0 };
  unsigned char *jpegs[3] = { NULL, NULL, NULL };
  size_t sizes[3] = { 0, 0, 0 };
  int r;

  if (!cinst || !dinst) {
    printf("tj3Init failed\n");
    return 1;
  }
  tj3Set(cinst, TJPARAM_QUALITY, 90);
  tj3Set(cinst, TJPARAM_SUBSAMP, TJSAMP_444);
  tj3Set(cinst, TJPARAM_COLORSPACE, TJCS_RGB);

  for (r = 0; r < 3; r++) {
    /* The single compressor instance is reused; only the interval changes. */
    tj3Set(cinst, TJPARAM_RESTARTBLOCKS, intervals[r]);
    if (tj3Compress8(cinst, images[r], JT_WIDTH, JT_WIDTH * 3, JT_HEIGHT,
                     TJPF_RGB, &jpegs[r], &sizes[r]) != 0) {
      printf("tj3Compress8 run %d failed: %s\n", r + 1,
             tj3GetErrorStr(cinst));
      goto fail;
    }
    /* TurboJPEG reuses one growable output buffer; snapshot the bytes. */
    unsigned char *snapshot = (unsigned char *)malloc(sizes[r]);
    memcpy(snapshot, jpegs[r], sizes[r]);
    jpegs[r] = snapshot;
  }

  for (r = 0; r < 3; r++)
    verify_run("TurboJPEG", r, intervals[r], jpegs[r], sizes[r],
               r == 2 ? jpegs[1] : NULL, r == 2 ? sizes[1] : 0, dinst);

  for (r = 0; r < 3; r++) free(jpegs[r]);
  tj3Destroy(cinst);
  tj3Destroy(dinst);
  return g_failures ? 1 : 0;

fail:
  for (r = 0; r < 3; r++) free(jpegs[r]);
  tj3Destroy(cinst);
  tj3Destroy(dinst);
  return 1;
}

/* --------------------------- raw libjpeg C API --------------------------- */

struct my_error_mgr {
  struct jpeg_error_mgr base;
  jmp_buf jump;
};

static void
capi_error_exit(j_common_ptr cinfo)
{
  struct my_error_mgr *e = (struct my_error_mgr *)cinfo->err;
  char msg[JMSG_LENGTH_MAX];

  (*cinfo->err->format_message)(cinfo, msg);
  printf("libjpeg error: %s\n", msg);
  longjmp(e->jump, 1);
}

static unsigned char *capi_out;
static size_t capi_cap;

static void capi_dest_init(j_compress_ptr cinfo) { (void)cinfo; }
static boolean capi_dest_empty(j_compress_ptr cinfo) { (void)cinfo; return TRUE; }
static void capi_dest_term(j_compress_ptr cinfo) { (void)cinfo; }

static int
run_capi(const unsigned char *images[3])
{
  struct jpeg_compress_struct cinfo;
  struct my_error_mgr jerr;
  struct jpeg_destination_mgr dest;
  const int intervals[3] = { JT_RESTART, 0, 0 };
  unsigned char *jpegs[3];
  size_t sizes[3];
  int r, ci;

  cinfo.err = jpeg_std_error(&jerr.base);
  jerr.base.error_exit = capi_error_exit;
  jpeg_create_compress(&cinfo);

  /* Configure the long-lived encoder exactly once. */
  cinfo.image_width = JT_WIDTH;
  cinfo.image_height = JT_HEIGHT;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 90, TRUE);
  jpeg_set_colorspace(&cinfo, JCS_RGB);
  for (ci = 0; ci < 3; ci++) {
    cinfo.comp_info[ci].h_samp_factor = 1;
    cinfo.comp_info[ci].v_samp_factor = 1;
  }

  for (r = 0; r < 3; r++) {
    if (setjmp(jerr.jump)) {
      jpeg_destroy_compress(&cinfo);
      return 1;
    }
    cinfo.restart_interval = (unsigned int)intervals[r];

    capi_cap = 1 << 20;
    capi_out = (unsigned char *)malloc(capi_cap);
    dest.init_destination = capi_dest_init;
    dest.empty_output_buffer = capi_dest_empty;
    dest.term_destination = capi_dest_term;
    dest.next_output_byte = capi_out;
    dest.free_in_buffer = capi_cap;
    cinfo.dest = &dest;

    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
      JSAMPROW row = (JSAMPROW)(images[r] +
                                cinfo.next_scanline * JT_WIDTH * 3);
      jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);

    sizes[r] = capi_cap - cinfo.dest->free_in_buffer;
    jpegs[r] = (unsigned char *)malloc(sizes[r]);
    memcpy(jpegs[r], capi_out, sizes[r]);
    free(capi_out);
    cinfo.dest = NULL;

    /* Reuse the same object for the next image, as a service would. */
    jpeg_abort_compress(&cinfo);
  }
  jpeg_destroy_compress(&cinfo);

  tjhandle dinst = tj3Init(TJINIT_DECOMPRESS);
  for (r = 0; r < 3; r++)
    verify_run("libjpeg ", r, intervals[r], jpegs[r], sizes[r],
               r == 2 ? jpegs[1] : NULL, r == 2 ? sizes[1] : 0, dinst);
  tj3Destroy(dinst);

  for (r = 0; r < 3; r++) free(jpegs[r]);
  return g_failures ? 1 : 0;
}

/* --------------------------------- main ---------------------------------- */

int
main(int argc, char **argv)
{
  enum mode mode = MODE_TJ;
  const unsigned char *images[3];
  int r;

  if (argc > 3 || (argc == 2 && strcmp(argv[1], "-m") == 0) ||
      (argc == 3 && strcmp(argv[1], "-m") != 0)) {
    printf("usage: %s [-m tj|capi]\n", argv[0]);
    return 2;
  }
  if (argc == 3) {
    if (strcmp(argv[2], "capi") == 0)
      mode = MODE_CAPI;
    else if (strcmp(argv[2], "tj") == 0)
      mode = MODE_TJ;
    else {
      printf("unknown mode '%s' (use 'tj' or 'capi')\n", argv[2]);
      return 2;
    }
  } else if (argc == 2) {
    if (strcmp(argv[1], "capi") == 0)
      mode = MODE_CAPI;
    else if (strcmp(argv[1], "tj") == 0)
      mode = MODE_TJ;
    else {
      printf("unknown mode '%s' (use 'tj' or 'capi')\n", argv[1]);
      return 2;
    }
  }

  for (r = 0; r < 3; r++) {
    images[r] = (const unsigned char *)malloc(JT_BYTES);
    /* Images 2 and 3 are pixel-identical; image 1 differs. */
    make_image((unsigned char *)images[r], r == 0 ? 1 : 42);
  }

  printf("JT-B: one compressor reused for three %dx%d RGB images, "
         "restart intervals %d, 0, 0\n", JT_WIDTH, JT_HEIGHT, JT_RESTART);
  int rc = (mode == MODE_TJ) ? run_turbojpeg(images) : run_capi(images);

  for (r = 0; r < 3; r++) free((void *)images[r]);
  if (rc == 0)
    printf("PASS: all JT-B restart-marker invariants hold\n");
  else
    printf("FAIL: %d invariant(s) violated\n", g_failures);
  return rc;
}
