/*
 * rastertointermec.c — CUPS raster filter for Intermec EasyCoder PF8t
 * =====================================================================
 * Reads a CUPS raster stream, converts each page to a 1-bit raw binary
 * bitmap, and emits ESim v7.x GW + P commands to stdout.
 *
 * Protocol: ESim v7.x / "ESim Classic"
 *           (ESim for the PC4 and PF8 Printers, P/N 937-011-xxx)
 * Hardware: Intermec EasyCoder PF8t — 203 dpi, DT/TT
 *
 * ESim commands used:
 *   LF              — mandatory sequence prefix (ASCII 0x0A)
 *   N LF            — clear image buffer
 *   D<n> LF         — density 0–15 (default 10)
 *   S<n> LF         — speed mm/sec: 50|75|100 (= 2|3|4 ips)
 *   q<n> LF         — label width in dots (max 832 at 203 dpi)
 *   Q<len>,<gap> LF — form length + inter-label gap in dots
 *   GW x,y,w,h,<DATA> — store 1-bit raw binary bitmap in image buffer
 *                        w = bytes per row, h = dot rows
 *                        DATA = w*h bytes of raw binary (no file formatting)
 *   P<n> LF         — print n copies
 *
 * ESim v7.x syntax rules:
 *   - Commands end with LF (ASCII 0x0A). CR is silently ignored inside
 *     CR+LF; bare CR alone does not work.
 *   - Always start a command sequence with a leading LF.
 *   - GW does NOT end with LF — printer counts bytes from w×h.
 *
 * Build:
 *   gcc -O2 -Wall -o rastertointermec rastertointermec.c -lcups
 *
 * CUPS invocation:
 *   rastertointermec <job-id> <user> <title> <copies> <options> [file]
 */

#include <cups/raster.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define LF               "\x0d\x0a"
#define ESIM_MAX_WIDTH   832    /* maximum label width in dots at 203 dpi */
#define ESIM_DPI         203
#define ESIM_GAP_DOTS    24     /* default inter-label gap in dots */
#define DEFAULT_DENSITY  10
#define DEFAULT_IPS      2

/* -------------------------------------------------------------------------
 * Logging helpers — match Python format: LEVEL [rastertointermec] message
 * ---------------------------------------------------------------------- */

#define log_debug(fmt, ...) \
    fprintf(stderr, "DEBUG [rastertointermec] " fmt "\n", ##__VA_ARGS__)

#define log_info(fmt, ...) \
    fprintf(stderr, "INFO [rastertointermec] " fmt "\n", ##__VA_ARGS__)

#define log_warning(fmt, ...) \
    fprintf(stderr, "WARNING [rastertointermec] " fmt "\n", ##__VA_ARGS__)

#define log_error(fmt, ...) \
    fprintf(stderr, "ERROR [rastertointermec] " fmt "\n", ##__VA_ARGS__)

/* -------------------------------------------------------------------------
 * Option parsing
 *
 * Parses a whitespace-separated string of "key=value" tokens (case-
 * insensitive keys).  Returns a heap-allocated value string for the given
 * key, or NULL if not found.  Caller must free() the result.
 * ---------------------------------------------------------------------- */

static char *parse_option(const char *options_str, const char *key)
{
    if (!options_str || !*options_str)
        return NULL;

    char *buf = strdup(options_str);
    if (!buf)
        return NULL;

    char *result = NULL;
    char *tok = strtok(buf, " \t");

    while (tok) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            /* Case-insensitive key comparison */
            char *k = tok;
            char lower_k[256];
            size_t i;
            for (i = 0; i < sizeof(lower_k) - 1 && k[i]; i++)
                lower_k[i] = (char)tolower((unsigned char)k[i]);
            lower_k[i] = '\0';

            if (strcmp(lower_k, key) == 0) {
                result = strdup(eq + 1);
                break;
            }
        }
        tok = strtok(NULL, " \t");
    }

    free(buf);
    return result;
}

/* -------------------------------------------------------------------------
 * get_density — extract darkness level (0–15) from options string
 *
 * Looks for "darkness=Darkness<n>" (or any value containing digits).
 * Defaults to DEFAULT_DENSITY if not found or not parseable.
 * ---------------------------------------------------------------------- */

static int get_density(const char *options_str)
{
    char *val = parse_option(options_str, "darkness");
    if (!val)
        return DEFAULT_DENSITY;

    /* Extract digits from the value (e.g. "Darkness10" → "10") */
    char digits[64] = {0};
    size_t di = 0;
    for (size_t i = 0; val[i] && di < sizeof(digits) - 1; i++) {
        if (val[i] >= '0' && val[i] <= '9')
            digits[di++] = val[i];
    }
    free(val);

    if (di == 0)
        return DEFAULT_DENSITY;

    int level = atoi(digits);
    if (level < 0)  level = 0;
    if (level > 15) level = 15;
    return level;
}

/* -------------------------------------------------------------------------
 * get_speed_select — extract ESim speed select value (0-4) from options
 *
 * Looks for "printspeed=Speed<n>" where <n> is the IPS value (2,3,4).
 * ESim S command takes a select value:
 *   S0=30mm/s  S1=40mm/s  S2=50mm/s  S3=75mm/s  S4=100mm/s
 * PPD Speed2=S2, Speed3=S3, Speed4=S4.  Default S2.
 * ---------------------------------------------------------------------- */

static int get_speed_select(const char *options_str)
{
    char *val = parse_option(options_str, "printspeed");
    int sel = DEFAULT_IPS;  /* default 2 */

    if (val) {
        char digits[64] = {0};
        size_t di = 0;
        for (size_t i = 0; val[i] && di < sizeof(digits) - 1; i++) {
            if (val[i] >= '0' && val[i] <= '9')
                digits[di++] = val[i];
        }
        if (di > 0)
            sel = atoi(digits);
        free(val);
    }

    if (sel < 0) sel = 0;
    if (sel > 4) sel = 4;
    return sel;
}

/* -------------------------------------------------------------------------
 * pts_to_dots — convert PostScript points to printer dots
 *
 * 1 pt = 1/72 inch; printer resolution = ESIM_DPI dots/inch.
 * ---------------------------------------------------------------------- */

static int pts_to_dots(unsigned pts)
{
    return (int)round((double)pts / 72.0 * (double)ESIM_DPI);
}

/* -------------------------------------------------------------------------
 * ESim command emitters
 *
 * All write directly to stdout (the CUPS filter output pipe).
 * Return 0 on success, -1 on write error.
 * ---------------------------------------------------------------------- */

static int emit_raw(const void *data, size_t len)
{
    if (fwrite(data, 1, len, stdout) != len)
        return -1;
    return 0;
}

static int emit_str(const char *s)
{
    return emit_raw(s, strlen(s));
}

/* LF — leading escape prefix */
static int esim_lf(void)
{
    return emit_str(LF);
}

/* D<n> LF — print density 0–15 */
static int esim_density(int level)
{
    char buf[32];
    if (level < 0)  level = 0;
    if (level > 15) level = 15;
    snprintf(buf, sizeof(buf), "D%d" LF, level);
    return emit_str(buf);
}

/* S<n> LF — speed select: 0=30, 1=40, 2=50, 3=75, 4=100 mm/s */
static int esim_speed(int sel)
{
    char buf[32];
    if (sel < 0) sel = 0;
    if (sel > 4) sel = 4;
    snprintf(buf, sizeof(buf), "S%d" LF, sel);
    return emit_str(buf);
}

/* q<n> LF — label width in dots (1–832) */
static int esim_label_width(unsigned dots)
{
    char buf[32];
    if (dots < 1)              dots = 1;
    if (dots > ESIM_MAX_WIDTH) dots = ESIM_MAX_WIDTH;
    snprintf(buf, sizeof(buf), "q%u" LF, dots);
    return emit_str(buf);
}

/* Q<len>,<gap> LF — form length and inter-label gap in dots */
static int esim_form_length(unsigned length_dots, unsigned gap_dots)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Q%u,%u" LF, length_dots, gap_dots);
    return emit_str(buf);
}

/* N LF — clear image buffer */
static int esim_clear(void)
{
    return emit_str("N" LF);
}

/*
 * GW x,y,w,h,<DATA> — store raw bitmap in image buffer.
 * NOTE: No trailing LF — printer counts exactly w*h bytes of pixel data.
 */
static int esim_gw(unsigned x, unsigned y,
                   unsigned width_bytes, unsigned height_dots,
                   const unsigned char *data)
{
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "GW%u,%u,%u,%u,",
             x, y, width_bytes, height_dots);
    if (emit_str(hdr) < 0)
        return -1;
    return emit_raw(data, (size_t)width_bytes * height_dots);
}

/* P<n> LF — print n copies */
static int esim_print(unsigned copies)
{
    char buf[32];
    if (copies < 1)    copies = 1;
    if (copies > 9999) copies = 9999;
    snprintf(buf, sizeof(buf), "P%u" LF, copies);
    return emit_str(buf);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* Validate argument count: 1 (test), 6 (normal CUPS), or 7 (file input) */
    if (argc != 1 && argc != 6 && argc != 7) {
        fprintf(stderr,
                "Usage: rastertointermec job-id user title copies options\n");
        return 1;
    }

    const char *job_id = "0";
    const char *title  = "test";
    unsigned    copies = 1;
    const char *options_str = NULL;

    if (argc >= 6) {
        job_id      = argv[1];
        title       = argv[3];
        copies      = (argv[4][0] && atoi(argv[4]) > 0)
                          ? (unsigned)atoi(argv[4]) : 1;
        options_str = argv[5];
    }

    log_info("Job %s — '%s' — %u copies", job_id, title, copies);

    /* Open raster stream from stdin or an explicit file (argc==7) */
    int fd;
    if (argc == 7) {
        FILE *f = fopen(argv[6], "rb");
        if (!f) {
            log_error("Cannot open input file: %s", argv[6]);
            return 1;
        }
        fd = fileno(f);
    } else {
        fd = 0; /* stdin */
    }

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) {
        log_error("cupsRasterOpen failed");
        return 1;
    }

    /* Derive ESim setup parameters from CUPS options */
    int density  = get_density(options_str);
    int speed    = get_speed_select(options_str);

    /* Auto-contrast: Off, Light (k=4), Medium (k=6), Strong (k=10) */
    double contrast_k = 0.0;  /* 0 = disabled */
    {
        char *val = parse_option(options_str, "autocontrast");
        if (val) {
            if (strcasecmp(val, "Light") == 0)       contrast_k = 4.0;
            else if (strcasecmp(val, "Medium") == 0)  contrast_k = 6.0;
            else if (strcasecmp(val, "Strong") == 0)  contrast_k = 10.0;
            free(val);
        }
    }

    log_debug("density=%d speed=S%d contrast=%s (k=%.0f)",
              density, speed, contrast_k > 0 ? "on" : "off", contrast_k);

    /* Set stdout to binary mode (important on Windows; harmless on POSIX) */
    /* On Linux/macOS this is a no-op but documents intent. */

    int page_num    = 0;
    int setup_sent  = 0;
    int rc          = 0;

    /* For deduplication: track previous page bitmap */
    unsigned char *prev_bitmap = NULL;
    size_t prev_bitmap_size = 0;
    unsigned prev_out_bpl = 0, prev_h_px = 0;
    int pending_copies = 0;

    cups_page_header2_t hdr;

    while (cupsRasterReadHeader2(ras, &hdr)) {
        page_num++;

        unsigned w_px  = hdr.cupsWidth;
        unsigned h_px  = hdr.cupsHeight;
        unsigned bpl   = hdr.cupsBytesPerLine;

        log_debug("Page %d: %ux%u px, %u bytes/line, bpc=%u, cs=%u",
                  page_num, w_px, h_px, bpl,
                  hdr.cupsBitsPerColor, hdr.cupsColorSpace);

        /* Validate pixel format */
        if (hdr.cupsBitsPerColor != 8 && hdr.cupsBitsPerColor != 1) {
            log_error("Page %d: expected 1 or 8 bits/color, got %u.",
                      page_num, hdr.cupsBitsPerColor);
            rc = 1;
            break;
        }
        /*
         * cupsColorSpace 3  = CUPS_CSPACE_K  (0=white, 255=black)
         * cupsColorSpace 18 = CUPS_CSPACE_SW (0=black, 255=white)
         */
        int invert_input = 0;
        if (hdr.cupsColorSpace == 18) {
            invert_input = 1;  /* SW: need to invert so 255=black */
        } else if (hdr.cupsColorSpace != 3) {
            log_warning("Page %d: unexpected cupsColorSpace %u",
                        page_num, hdr.cupsColorSpace);
        }

        int is_8bit = (hdr.cupsBitsPerColor == 8);

        /* Output bitmap: 1 bit per pixel, packed, ESim polarity */
        unsigned out_bpl = (w_px + 7) / 8;
        size_t bitmap_size = (size_t)out_bpl * h_px;
        unsigned char *bitmap = malloc(bitmap_size);
        if (!bitmap) {
            log_error("Page %d: out of memory (%zu bytes)", page_num, bitmap_size);
            rc = 1;
            break;
        }

        if (is_8bit) {
            /*
             * 8-bit grayscale pipeline:
             *   1. Read full image
             *   2. Auto-contrast: histogram stretch + midtone S-curve
             *   3. Atkinson dithering with serpentine scanning
             */

            /* Read full grayscale image */
            size_t gray_size = (size_t)w_px * h_px;
            unsigned char *gray = malloc(gray_size);
            if (!gray) {
                log_error("Page %d: out of memory (%zu bytes)", page_num, gray_size);
                free(bitmap);
                rc = 1;
                break;
            }

            unsigned char *row_buf = malloc(bpl);
            int read_ok = 1;
            for (unsigned row = 0; row < h_px; row++) {
                if (cupsRasterReadPixels(ras, row_buf, bpl) != bpl) {
                    log_error("Page %d: short read on row %u", page_num, row);
                    read_ok = 0;
                    break;
                }
                /* Normalize: convert to 0=white, 255=black regardless of colorspace */
                unsigned char *dst = gray + (size_t)row * w_px;
                for (unsigned x = 0; x < w_px; x++)
                    dst[x] = invert_input ? (255 - row_buf[x]) : row_buf[x];
            }
            free(row_buf);

            if (!read_ok) {
                free(gray); free(bitmap);
                rc = 1;
                break;
            }

            /*
             * Auto-contrast (optional): histogram percentile stretch + S-curve
             *
             * 1. Build histogram
             * 2. Find 1st and 99th percentile values (ignore outliers)
             * 3. Linear stretch so [lo, hi] → [0, 255]
             * 4. Apply S-curve to expand midtone separation:
             *    f(x) = 255 / (1 + exp(-k*(x/255 - 0.5)))
             *    with k≈6 for gentle enhancement
             */
            if (contrast_k > 0.0) {
                unsigned hist[256] = {0};
                for (size_t i = 0; i < gray_size; i++)
                    hist[gray[i]]++;

                /* Find 1st and 99th percentile */
                size_t total = gray_size;
                size_t thresh_lo = total / 100;
                size_t thresh_hi = total * 99 / 100;
                size_t cum = 0;
                int lo = 0, hi = 255;
                for (int i = 0; i < 256; i++) {
                    cum += hist[i];
                    if (cum >= thresh_lo && lo == 0) lo = i;
                    if (cum >= thresh_hi) { hi = i; break; }
                }

                if (hi <= lo) { lo = 0; hi = 255; }

                log_debug("Auto-contrast: stretch [%d, %d] → [0, 255], k=%.0f",
                          lo, hi, contrast_k);

                /* Build LUT: percentile stretch + S-curve */
                unsigned char lut[256];
                double s0 = 1.0 / (1.0 + exp(-contrast_k * -0.5));
                double s1 = 1.0 / (1.0 + exp(-contrast_k *  0.5));
                for (int i = 0; i < 256; i++) {
                    double v = (double)(i - lo) / (double)(hi - lo);
                    if (v < 0.0) v = 0.0;
                    if (v > 1.0) v = 1.0;

                    v = 1.0 / (1.0 + exp(-contrast_k * (v - 0.5)));
                    v = (v - s0) / (s1 - s0);

                    int val = (int)(v * 255.0 + 0.5);
                    if (val < 0)   val = 0;
                    if (val > 255) val = 255;
                    lut[i] = (unsigned char)val;
                }

                for (size_t i = 0; i < gray_size; i++)
                    gray[i] = lut[gray[i]];
            }

            /*
             * Atkinson dithering with serpentine scanning
             *
             * Diffuses 6/8 of error to 6 neighbors (each gets err/8).
             * Kernel:
             *   . * 1 1
             *   1 1 1 .
             *   . 1 . .
             */
            int *cur_err = calloc(w_px + 4, sizeof(int));
            int *nxt_err = calloc(w_px + 4, sizeof(int));
            int *nn_err  = calloc(w_px + 4, sizeof(int));

            if (!cur_err || !nxt_err || !nn_err) {
                log_error("Page %d: out of memory for dithering", page_num);
                free(gray); free(bitmap);
                free(cur_err); free(nxt_err); free(nn_err);
                rc = 1;
                break;
            }

            for (unsigned row = 0; row < h_px; row++) {
                unsigned char *row_in = gray + (size_t)row * w_px;

                unsigned char *out = bitmap + (size_t)row * out_bpl;
                memset(out, 0, out_bpl);
                memset(nn_err, 0, (w_px + 4) * sizeof(int));

                int left_to_right = !(row & 1);  /* serpentine */

                if (left_to_right) {
                    for (unsigned x = 0; x < w_px; x++) {
                        int old_val = (int)row_in[x] + cur_err[x + 2];
                        if (old_val < 0)   old_val = 0;
                        if (old_val > 255) old_val = 255;

                        int new_val;
                        if (old_val >= 128) {
                            new_val = 255;
                        } else {
                            new_val = 0;
                            out[x / 8] |= (0x80 >> (x % 8));
                        }

                        int e = (old_val - new_val) / 8;
                        cur_err[x + 3] += e;    /* right 1 */
                        cur_err[x + 4] += e;    /* right 2 */
                        nxt_err[x + 1] += e;    /* below-left */
                        nxt_err[x + 2] += e;    /* below */
                        nxt_err[x + 3] += e;    /* below-right */
                        nn_err[x + 2]  += e;    /* 2 below */
                    }
                } else {
                    for (int x = (int)w_px - 1; x >= 0; x--) {
                        int old_val = (int)row_in[x] + cur_err[x + 2];
                        if (old_val < 0)   old_val = 0;
                        if (old_val > 255) old_val = 255;

                        int new_val;
                        if (old_val >= 128) {
                            new_val = 255;
                        } else {
                            new_val = 0;
                            out[x / 8] |= (0x80 >> (x % 8));
                        }

                        int e = (old_val - new_val) / 8;
                        cur_err[x + 1] += e;    /* left 1 */
                        cur_err[x]     += e;    /* left 2 */
                        nxt_err[x + 3] += e;    /* below-right */
                        nxt_err[x + 2] += e;    /* below */
                        nxt_err[x + 1] += e;    /* below-left */
                        nn_err[x + 2]  += e;    /* 2 below */
                    }
                }

                /* Rotate error buffers: cur←nxt, nxt←nn, nn←fresh */
                int *tmp = cur_err;
                cur_err = nxt_err;
                nxt_err = nn_err;
                nn_err = tmp;
            }

            free(cur_err);
            free(nxt_err);
            free(nn_err);
            free(gray);
        } else {
            /*
             * 1-bit input: read and adjust polarity for ESim.
             * ESim: 0=black (printed), 1=white
             * CUPS K (cs=3):  1=black → XOR 0xFF to get ESim polarity
             * CUPS SW (cs=18): 0=black → already ESim polarity, no XOR
             */
            int read_ok = 1;
            for (unsigned row = 0; row < h_px; row++) {
                unsigned char *dest = bitmap + (size_t)row * out_bpl;
                if (cupsRasterReadPixels(ras, dest, bpl) != bpl) {
                    log_error("Page %d: short read on row %u", page_num, row);
                    read_ok = 0;
                    break;
                }
                if (!invert_input) {
                    /* K colorspace: invert bits */
                    for (unsigned b = 0; b < bpl; b++)
                        dest[b] ^= 0xFF;
                }
            }

            if (!read_ok) {
                free(bitmap);
                rc = 1;
                break;
            }
        }

        /*
         * Emit one-time printer setup on the first page.
         * The leading LF is the mandatory ESim sequence prefix.
         */
        if (!setup_sent) {
            unsigned h_pts  = hdr.PageSize[1];   /* page height in points */
            unsigned h_dots = (h_pts > 10)
                                  ? (unsigned)pts_to_dots(h_pts)
                                  : h_px;

            esim_lf();
            esim_density(density);
            esim_speed(speed);
            esim_label_width(w_px);
            esim_form_length(h_dots, ESIM_GAP_DOTS);
            setup_sent = 1;

            log_debug("Setup: density=%d speed=S%d w=%u h_dots=%u gap=%d",
                      density, speed, w_px, h_dots,
                      ESIM_GAP_DOTS);
        }

        /*
         * Deduplication: if this page is identical to the previous one,
         * just accumulate copies instead of re-sending the bitmap.
         * cgpdftoraster sends N identical pages for N copies.
         */
        int is_dup = 0;
        if (prev_bitmap && prev_bitmap_size == bitmap_size &&
            prev_out_bpl == out_bpl && prev_h_px == h_px &&
            memcmp(prev_bitmap, bitmap, bitmap_size) == 0) {
            is_dup = 1;
            pending_copies++;
            log_debug("Page %d is duplicate, accumulating (%d copies pending)",
                      page_num, pending_copies);
            free(bitmap);
        } else {
            /* Flush previous page if pending */
            if (prev_bitmap && pending_copies > 0) {
                esim_lf();
                esim_clear();
                esim_gw(0, 0, prev_out_bpl, prev_h_px, prev_bitmap);
                esim_print(pending_copies);
                fflush(stdout);
                log_debug("Flushed previous page: GW 0,0,%u,%u + P%u",
                          prev_out_bpl, prev_h_px, pending_copies);
            }
            free(prev_bitmap);
            prev_bitmap = bitmap;
            prev_bitmap_size = bitmap_size;
            prev_out_bpl = out_bpl;
            prev_h_px = h_px;
            pending_copies = 1;
        }
        (void)is_dup;
    }

    /* Flush last pending page */
    if (prev_bitmap && pending_copies > 0) {
        esim_lf();
        esim_clear();
        esim_gw(0, 0, prev_out_bpl, prev_h_px, prev_bitmap);
        esim_print(pending_copies);
        fflush(stdout);
        log_debug("Final page: GW 0,0,%u,%u + P%u",
                  prev_out_bpl, prev_h_px, pending_copies);
    }
    free(prev_bitmap);

    cupsRasterClose(ras);

    if (page_num == 0 && rc == 0)
        log_info("No pages received.");
    else
        log_info("Done. %d page(s) processed.", page_num);

    return rc;
}
