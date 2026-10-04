/* picture.c: a page's pictures. The decoders feed rows to a box filter that
   shrinks them to the size they're shown at while they're decoded: a large
   JPEG is also decoded at 1/2, 1/4 or 1/8 of its size by libjpeg itself. The
   result is in a format the PSP draws directly: 16 bits a pixel, dithered,
   or 32 for pictures with transparent parts (logos and icons, mostly, whose
   flat colours dithering would speckle). */
#include <ctype.h>
#include <malloc.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <jpeglib.h>
#include <png.h>
#include <gif_lib.h>
#include "picture.h"
#include "css.h"

#define FRAME_BYTES_MAX (4 * 1024 * 1024)       /* a frame held whole (an interlaced PNG, a GIF) */
#define PNG_PIXELS_MAX (4 * 1024 * 1024)        /* a PNG decoded row by row: about a second on a PSP */
#define JPEG_MEMORY_MAX (6 * 1024 * 1024)       /* a progressive JPEG's coefficients */
#define SHRINK_MAX 128                          /* source pixels a side per decoded pixel */
#define BARRIER() __asm__ __volatile__("" ::: "memory")

/* ---- shrinking ---- */

/* Averages source rows into a target of tw x th RGBA pixels (alpha-weighted,
   so transparent pixels don't darken the edges). */
typedef struct {
    int sw, sh, tw, th, row, seen, alpha;
    int *column;                /* target column of each source column */
    uint32_t *sums;             /* r*a, g*a, b*a, a and a count, for the target row */
    unsigned char *out;
} shrinker;

static int shrink_start(shrinker *s, int sw, int sh, int tw, int th)
{
    memset(s, 0, sizeof(*s));
    /* never enlarged, and at most SHRINK_MAX x SHRINK_MAX pixels in a sum:
       they stay within 32 bits */
    if (tw > sw) tw = sw;
    if (th > sh) th = sh;
    if (tw < (sw + SHRINK_MAX - 1) / SHRINK_MAX) tw = (sw + SHRINK_MAX - 1) / SHRINK_MAX;
    if (th < (sh + SHRINK_MAX - 1) / SHRINK_MAX) th = (sh + SHRINK_MAX - 1) / SHRINK_MAX;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    s->sw = sw; s->sh = sh; s->tw = tw; s->th = th;
    s->column = malloc((size_t)sw * sizeof(int));
    s->sums = calloc((size_t)tw * 5, sizeof(uint32_t));
    s->out = malloc((size_t)tw * th * 4);
    if (!s->column || !s->sums || !s->out) return -1;
    for (int x = 0; x < sw; x++) s->column[x] = (int)((int64_t)x * tw / sw);
    return 0;
}
static void shrink_end(shrinker *s)
{
    free(s->column); free(s->sums); free(s->out);
    memset(s, 0, sizeof(*s));
}
static void emit_row(shrinker *s)
{
    if (s->row >= s->th) return;
    unsigned char *o = s->out + (size_t)s->row * s->tw * 4;
    for (int x = 0; x < s->tw; x++, o += 4) {
        uint32_t *t = &s->sums[x * 5];
        if (!t[4]) { memset(o, 0, 4); continue; }
        unsigned a = t[3] / t[4];
        if (t[3]) { o[0] = (unsigned char)(t[0] / t[3]); o[1] = (unsigned char)(t[1] / t[3]); o[2] = (unsigned char)(t[2] / t[3]); }
        else o[0] = o[1] = o[2] = 0;
        o[3] = (unsigned char)a;
        if (a < 250) s->alpha = 1;
    }
    memset(s->sums, 0, (size_t)s->tw * 5 * sizeof(uint32_t));
}
/* One source row: RGBA when `channels` is 4, RGB when 3. */
static void shrink_row(shrinker *s, const unsigned char *p, int channels)
{
    int target = (int)((int64_t)s->seen * s->th / s->sh);
    if (target != s->row) { emit_row(s); s->row = target; }
    for (int x = 0; x < s->sw; x++, p += channels) {
        uint32_t *t = &s->sums[s->column[x] * 5], a = channels == 4 ? p[3] : 255;
        t[0] += p[0] * a; t[1] += p[1] * a; t[2] += p[2] * a; t[3] += a; t[4]++;
    }
    s->seen++;
}

/* Transparent pixels take their neighbours' colour, so that smooth scaling
   doesn't draw a dark fringe around what's beside them. */
static void bleed(unsigned char *p, int w, int h)
{
    static const int dx[] = {-1, 1, 0, 0}, dy[] = {0, 0, -1, 1};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *c = p + ((size_t)y * w + x) * 4;
            if (c[3]) continue;
            int r = 0, g = 0, b = 0, n = 0;
            for (int k = 0; k < 4; k++) {
                int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const unsigned char *q = p + ((size_t)ny * w + nx) * 4;
                if (!q[3]) continue;
                r += q[0]; g += q[1]; b += q[2]; n++;
            }
            if (n) { c[0] = (unsigned char)(r / n); c[1] = (unsigned char)(g / n); c[2] = (unsigned char)(b / n); }
        }
}

static int up(int value, int add) { value += add; return value > 255 ? 255 : value; }

/* The shrunk RGBA pixels as the PSP's: with alpha, as they are (its 32-bit
   format has them in this order); else in 16 bits with ordered dithering,
   so that gradients don't band. */
static int finish(shrinker *s, picture_image *out)
{
    static const unsigned char bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    emit_row(s);
    int w = s->tw, h = s->th, stride = (w + 1 + 7) & ~7, format = s->alpha ? PIXELS_8888 : PIXELS_5650, size = format == PIXELS_8888 ? 4 : 2;
    unsigned char *pixels = memalign(16, (size_t)stride * (h + 1) * size);
    if (!pixels) return -1;
    if (format == PIXELS_8888) {
        bleed(s->out, w, h);
        for (int y = 0; y < h; y++) {
            uint32_t *o = (uint32_t *)(pixels + (size_t)y * stride * 4);
            memcpy(o, s->out + (size_t)y * w * 4, (size_t)w * 4);
            for (int x = w; x < stride; x++) o[x] = o[w - 1];
        }
    } else
        for (int y = 0; y < h; y++) {
            const unsigned char *c = s->out + (size_t)y * w * 4;
            unsigned short *o = (unsigned short *)(pixels + (size_t)y * stride * 2);
            for (int x = 0; x < w; x++, c += 4) {
                int k = bayer[y & 3][x & 3];
                o[x] = (unsigned short)((up(c[2], k >> 1) >> 3) << 11 | (up(c[1], k >> 2) >> 2) << 5 | up(c[0], k >> 1) >> 3);
            }
            for (int x = w; x < stride; x++) o[x] = o[w - 1];
        }
    memcpy(pixels + (size_t)h * stride * size, pixels + (size_t)(h - 1) * stride * size, (size_t)stride * size);
    out->pixels = pixels; out->w = w; out->h = h; out->stride = stride; out->format = format;
    return 0;
}

/* The size a w x h picture is decoded at within max_w x max_h (0: any),
   never enlarged. */
static void fit(int w, int h, int max_w, int max_h, int stretch, int *tw, int *th)
{
    if (max_w <= 0 || max_w > PICTURE_SIDE_MAX) max_w = PICTURE_SIDE_MAX;
    if (max_h <= 0 || max_h > PICTURE_SIDE_MAX) max_h = PICTURE_SIDE_MAX;
    *tw = w; *th = h;
    if (stretch) {
        if (*tw > max_w) *tw = max_w;
        if (*th > max_h) *th = max_h;
    } else {
        if (*tw > max_w) { *th = (int)((int64_t)*th * max_w / *tw); *tw = max_w; }
        if (*th > max_h) { *tw = (int)((int64_t)*tw * max_h / *th); *th = max_h; }
    }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

/* ---- JPEG ---- */

typedef struct { struct jpeg_error_mgr base; jmp_buf jump; } jpeg_failure;
static void jpeg_fail(j_common_ptr c) { longjmp(((jpeg_failure *)c->err)->jump, 1); }
static void jpeg_quiet(j_common_ptr c) { (void)c; }

static int decode_jpeg(const unsigned char *data, size_t length, int tw, int th, picture_image *out)
{
    struct jpeg_decompress_struct c;
    jpeg_failure failure;
    shrinker s;
    unsigned char *volatile row = NULL;
    memset(&s, 0, sizeof(s));
    c.err = jpeg_std_error(&failure.base);
    failure.base.error_exit = jpeg_fail;
    failure.base.output_message = jpeg_quiet;
    if (setjmp(failure.jump)) {
        jpeg_destroy_decompress(&c);
        free(row); shrink_end(&s);
        return -1;
    }
    jpeg_create_decompress(&c);
    c.mem->max_memory_to_use = JPEG_MEMORY_MAX + 1024 * 1024;
    jpeg_mem_src(&c, (unsigned char *)data, (unsigned long)length);
    jpeg_read_header(&c, TRUE);
    int w = (int)c.image_width, h = (int)c.image_height;
    if (c.progressive_mode) {
        /* its scans refine the whole picture: all its coefficients are kept */
        size_t need = 0;
        for (int i = 0; i < c.num_components; i++) need += (size_t)c.comp_info[i].width_in_blocks * c.comp_info[i].height_in_blocks * 128;
        if (need > JPEG_MEMORY_MAX) longjmp(failure.jump, 1);
    }
    /* libjpeg decodes at 1/2, 1/4 or 1/8 of the size at a fraction of the cost */
    c.scale_num = 1; c.scale_denom = 1;
    for (int d = 8; d > 1; d /= 2)
        if ((w + d - 1) / d >= tw && (h + d - 1) / d >= th) { c.scale_denom = (unsigned)d; break; }
    c.out_color_space = JCS_RGB;
    c.dct_method = JDCT_IFAST;
    c.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&c);
    if (c.output_components != 3 || shrink_start(&s, (int)c.output_width, (int)c.output_height, tw, th) < 0) longjmp(failure.jump, 1);
    row = malloc((size_t)c.output_width * 3);
    if (!row) longjmp(failure.jump, 1);
    while (c.output_scanline < c.output_height) {
        JSAMPROW rows[1] = { row };
        if (jpeg_read_scanlines(&c, rows, 1) != 1) longjmp(failure.jump, 1);
        shrink_row(&s, row, 3);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    free(row);
    int result = finish(&s, out);
    shrink_end(&s);
    return result;
}

/* ---- PNG ---- */

typedef struct { const unsigned char *data; size_t length, at; } memory_reader;
static void png_read_memory(png_structp png, png_bytep out, png_size_t n)
{
    memory_reader *r = png_get_io_ptr(png);
    if (n > r->length - r->at) png_error(png, "truncated");
    memcpy(out, r->data + r->at, n);
    r->at += n;
}
static void png_fail(png_structp png, png_const_charp message) { (void)message; png_longjmp(png, 1); }
static void png_quiet(png_structp png, png_const_charp message) { (void)png; (void)message; }

static int decode_png(const unsigned char *data, size_t length, int tw, int th, picture_image *out)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_fail, png_quiet);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    memory_reader reader = { data, length, 0 };
    unsigned char *volatile frame = NULL;
    shrinker s;
    memset(&s, 0, sizeof(s));
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return -1; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        free(frame); shrink_end(&s);
        return -1;
    }
    png_set_read_fn(png, &reader, png_read_memory);
    png_set_user_limits(png, 16384, 16384);
#ifdef PNG_SKIP_sRGB_CHECK_PROFILE
    png_set_option(png, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON);
#endif
    png_read_info(png, info);
    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    int type = png_get_color_type(png, info), interlaced = png_get_interlace_type(png, info) != PNG_INTERLACE_NONE;
    if ((uint64_t)w * h > PNG_PIXELS_MAX) png_error(png, "too large");
    png_set_expand(png);
    png_set_strip_16(png);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != (size_t)w * 4) png_error(png, "format");
    if (shrink_start(&s, (int)w, (int)h, tw, th) < 0) png_error(png, "memory");
    if (interlaced) {
        /* the passes fill the whole frame before its rows are complete */
        if ((uint64_t)w * h * 4 > FRAME_BYTES_MAX) png_error(png, "too large");
        frame = malloc((size_t)w * h * 4);
        if (!frame) png_error(png, "memory");
        for (int pass = 0; pass < passes; pass++)
            for (png_uint_32 y = 0; y < h; y++) png_read_row(png, frame + (size_t)y * w * 4, NULL);
        for (png_uint_32 y = 0; y < h; y++) shrink_row(&s, frame + (size_t)y * w * 4, 4);
    } else {
        frame = malloc((size_t)w * 4);
        if (!frame) png_error(png, "memory");
        for (png_uint_32 y = 0; y < h; y++) { png_read_row(png, frame, NULL); shrink_row(&s, frame, 4); }
    }
    png_destroy_read_struct(&png, &info, NULL);
    free(frame);
    int result = finish(&s, out);
    shrink_end(&s);
    return result;
}

/* ---- GIF: the first frame ---- */

static int gif_read(GifFileType *g, GifByteType *out, int n)
{
    memory_reader *r = g->UserData;
    if (n < 0 || (size_t)n > r->length - r->at) n = (int)(r->length - r->at);
    memcpy(out, r->data + r->at, (size_t)n);
    r->at += (size_t)n;
    return n;
}

static int decode_gif(const unsigned char *data, size_t length, int tw, int th, picture_image *out)
{
    memory_reader reader = { data, length, 0 };
    int error = 0, transparent = -1, result = -1;
    GifFileType *g = DGifOpen(&reader, gif_read, &error);
    if (!g) return -1;
    GifRecordType record;
    unsigned char *index = NULL, *line = NULL;
    shrinker s;
    memset(&s, 0, sizeof(s));
    while (DGifGetRecordType(g, &record) == GIF_OK && record != TERMINATE_RECORD_TYPE) {
        if (record == EXTENSION_RECORD_TYPE) {
            int code;
            GifByteType *block;
            if (DGifGetExtension(g, &code, &block) != GIF_OK) break;
            if (code == GRAPHICS_EXT_FUNC_CODE && block && block[0] >= 4 && (block[1] & 1)) transparent = block[4];
            while (block && DGifGetExtensionNext(g, &block) == GIF_OK && block) {}
            continue;
        }
        if (record != IMAGE_DESC_RECORD_TYPE || DGifGetImageDesc(g) != GIF_OK) break;
        const GifImageDesc *d = &g->Image;
        const ColorMapObject *map = d->ColorMap ? d->ColorMap : g->SColorMap;
        int sw = g->SWidth, sh = g->SHeight;
        if (sw <= 0 || sh <= 0 || (int64_t)sw * sh > FRAME_BYTES_MAX || !map || d->Width <= 0 || d->Height <= 0 ||
            (int64_t)d->Width * d->Height > FRAME_BYTES_MAX) break;
        /* the frame's lines, in order once interlacing is undone */
        index = malloc((size_t)d->Width * d->Height);
        if (!index) break;
        static const int start[] = {0, 4, 2, 1}, step[] = {8, 8, 4, 2};
        int ok = 1;
        if (d->Interlace) {
            for (int pass = 0; pass < 4 && ok; pass++)
                for (int y = start[pass]; y < d->Height && ok; y += step[pass]) ok = DGifGetLine(g, index + (size_t)y * d->Width, d->Width) == GIF_OK;
        } else for (int y = 0; y < d->Height && ok; y++) ok = DGifGetLine(g, index + (size_t)y * d->Width, d->Width) == GIF_OK;
        if (!ok) break;
        line = malloc((size_t)sw * 4);
        if (!line || shrink_start(&s, sw, sh, tw, th) < 0) break;
        for (int y = 0; y < sh; y++) {
            memset(line, 0, (size_t)sw * 4);
            int fy = y - d->Top;
            if (fy >= 0 && fy < d->Height)
                for (int x = 0; x < d->Width; x++) {
                    int sx = x + d->Left, k = index[(size_t)fy * d->Width + x];
                    if (sx < 0 || sx >= sw || k == transparent || k >= map->ColorCount) continue;
                    unsigned char *o = line + sx * 4;
                    o[0] = map->Colors[k].Red; o[1] = map->Colors[k].Green; o[2] = map->Colors[k].Blue; o[3] = 255;
                }
            shrink_row(&s, line, 4);
        }
        result = finish(&s, out);
        break;
    }
    free(index); free(line); shrink_end(&s);
    DGifCloseFile(g, &error);
    return result;
}

/* ---- formats ---- */

static int is_jpeg(const unsigned char *p, size_t n) { return n > 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF; }
static int is_png(const unsigned char *p, size_t n) { return n > 32 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8) && !memcmp(p + 12, "IHDR", 4); }
static int is_gif(const unsigned char *p, size_t n) { return n > 10 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6)); }

/* A JPEG's frame header: its size, and the memory its coefficients take
   when it's progressive. */
static int jpeg_frame(const unsigned char *p, size_t n, int *w, int *h, size_t *coefficients)
{
    *coefficients = 0;
    for (size_t i = 2; i + 9 < n;) {
        if (p[i] != 0xFF) return -1;
        unsigned marker = p[i + 1];
        if (marker == 0xFF) { i++; continue; }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { i += 2; continue; }
        size_t size = (size_t)(p[i + 2] << 8 | p[i + 3]);
        if (size < 2) return -1;
        /* SOF0 to SOF15, but not DHT, JPG or DAC */
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            *h = p[i + 5] << 8 | p[i + 6]; *w = p[i + 7] << 8 | p[i + 8];
            int components = p[i + 9], hmax = 1, vmax = 1;
            if (i + 10 + (size_t)components * 3 > n) return 0;
            for (int k = 0; k < components; k++) {
                int f = p[i + 11 + k * 3];
                if (f >> 4 > hmax) hmax = f >> 4;
                if ((f & 15) > vmax) vmax = f & 15;
            }
            if (marker == 0xC2 || marker == 0xC6 || marker == 0xCA || marker == 0xCE)
                for (int k = 0; k < components; k++) {
                    int f = p[i + 11 + k * 3], hs = f >> 4 ? f >> 4 : 1, vs = f & 15 ? f & 15 : 1;
                    size_t bw = ((size_t)*w * hs + hmax * 8 - 1) / (hmax * 8), bh = ((size_t)*h * vs + vmax * 8 - 1) / (vmax * 8);
                    *coefficients += bw * bh * 128;
                }
            return 0;
        }
        i += 2 + size;
    }
    return -1;
}

int picture_size(const unsigned char *p, size_t n, int *w, int *h)
{
    size_t coefficients;
    *w = *h = 0;
    if (is_png(p, n)) {
        uint32_t pw = (uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19];
        uint32_t ph = (uint32_t)p[20] << 24 | (uint32_t)p[21] << 16 | (uint32_t)p[22] << 8 | p[23];
        if (pw > 16384 || ph > 16384) return -1;
        *w = (int)pw; *h = (int)ph;
    }
    else if (is_gif(p, n)) { *w = p[6] | p[7] << 8; *h = p[8] | p[9] << 8; }
    else if (is_jpeg(p, n) && jpeg_frame(p, n, w, h, &coefficients) < 0) return -1;
    return *w > 0 && *h > 0 && *w <= 16384 && *h <= 16384 ? 0 : -1;
}

size_t picture_decode_memory(const unsigned char *p, size_t n)
{
    int w, h;
    size_t coefficients = 0;
    if (picture_size(p, n, &w, &h) < 0) return 0;
    /* the shrunk RGBA picture and the decoder's own buffers */
    size_t need = (size_t)(w < PICTURE_SIDE_MAX ? w : PICTURE_SIDE_MAX) * (h < PICTURE_SIDE_MAX ? h : PICTURE_SIDE_MAX) * 4 + 256 * 1024;
    if (is_jpeg(p, n) && jpeg_frame(p, n, &w, &h, &coefficients) == 0) need += coefficients;
    else if (is_png(p, n) && p[28]) need += (size_t)w * h * 4;     /* interlaced */
    else if (is_gif(p, n)) need += (size_t)w * h;
    return need;
}

int picture_decode(const unsigned char *data, size_t length, int max_w, int max_h, int stretch,
                   picture_image *out, int *natural_w, int *natural_h)
{
    memset(out, 0, sizeof(*out));
    *natural_w = *natural_h = 0;
    int w = 0, h = 0, tw, th, result = -1;
    if (picture_size(data, length, &w, &h) < 0) return -1;
    *natural_w = w; *natural_h = h;
    fit(w, h, max_w, max_h, stretch, &tw, &th);
    if (is_jpeg(data, length)) result = decode_jpeg(data, length, tw, th, out);
    else if (is_png(data, length)) result = decode_png(data, length, tw, th, out);
    else result = decode_gif(data, length, tw, th, out);
    if (result < 0) picture_free(out);
    return result;
}

void picture_free(picture_image *image)
{
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}

size_t picture_bytes(const picture_image *image)
{
    return image->pixels ? (size_t)image->stride * (image->h + 1) * (image->format == PIXELS_8888 ? 4 : 2) : 0;
}

static int hex(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

/* The picture types a data: URI may hold for this to decode it. */
static int data_picture(const char *uri)
{
    static const char *types[] = {"data:image/jpeg", "data:image/jpg", "data:image/pjpeg", "data:image/png", "data:image/gif"};
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); i++) {
        size_t n = strlen(types[i]);
        if (!strncasecmp(uri, types[i], n) && (uri[n] == ';' || uri[n] == ',')) return 1;
    }
    return 0;
}

unsigned char *picture_data(const char *uri, size_t *length)
{
    const char *comma = strchr(uri, ',');
    if (!data_picture(uri) || !comma || comma - uri < 7 || strncasecmp(comma - 7, ";base64", 7)) return NULL;
    const char *p = comma + 1;
    unsigned char *out = malloc(strlen(p) / 4 * 3 + 4);
    if (!out) return NULL;
    size_t used = 0;
    unsigned value = 0;
    int bits = 0;
    for (; *p; p++) {
        int c = (unsigned char)*p;
        /* an address may escape them */
        if (c == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { c = hex(p[1]) << 4 | hex(p[2]); p += 2; }
        if (c == '=') break;
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
                c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
        if (d < 0) { if (isspace(c)) continue; free(out); return NULL; }
        value = value << 6 | (unsigned)d;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[used++] = (unsigned char)(value >> bits); }
    }
    *length = used;
    return out;
}

/* ---- sources ---- */

static int same(const char *a, const char *b)
{
    while (*a && *b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return !*a && !*b;
}
static int has_tag(const dom_node *n, const char *tag) { return same(n->tag, tag); }

/* A format this can't decode, from the address's extension or data: type. */
static int undecodable(const char *url, size_t n)
{
    static const char *types[] = {".svg", ".svgz", ".webp", ".avif", ".jxl", ".heic", ".heif", ".bmp", ".ico", ".tif", ".tiff"};
    if (n >= 5 && !strncasecmp(url, "data:", 5)) return !data_picture(url);
    size_t end = 0;
    while (end < n && url[end] != '?' && url[end] != '#') end++;
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); i++) {
        size_t k = strlen(types[i]);
        if (end >= k && !strncasecmp(url + end - k, types[i], k)) return 1;
    }
    return 0;
}

/* The srcset candidate for a box `width` page pixels wide (as many device
   pixels): by width descriptor, the smallest wide enough (else the widest),
   by density 1x or the least above it. */
static const char *candidate(const char *set, int width, size_t *length, float *density)
{
    const char *best = NULL, *p = set;
    size_t best_length = 0;
    float best_score = 0, best_density = 1;
    int best_fits = 0;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *url = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t n = (size_t)(p - url);
        int ended = n && url[n - 1] == ',';
        while (n && url[n - 1] == ',') n--;
        float value = 1;
        char kind = 'x';
        if (!ended) {
            while (isspace((unsigned char)*p)) p++;
            const char *d = p;
            while (*p && *p != ',') p++;
            char *end;
            float v = strtof(d, &end);
            if (end > d && (*end == 'w' || *end == 'x') && v > 0) { value = v; kind = *end; }
        }
        if (!n || undecodable(url, n)) continue;
        float score = kind == 'w' ? value : value * 100000;
        int fits = kind == 'w' ? value >= width : value >= 1;
        if (!best || (fits && (!best_fits || score < best_score)) || (!fits && !best_fits && score > best_score)) {
            best = url; best_length = n; best_score = score; best_fits = fits;
            best_density = kind == 'x' ? value : 1;
        }
    }
    *length = best_length;
    *density = best_density;
    return best;
}

static int decodable_type(const char *type)
{
    static const char *types[] = {"", "image/jpeg", "image/jpg", "image/pjpeg", "image/png", "image/gif"};
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); i++) if (same(type, types[i])) return 1;
    return 0;
}

/* A placeholder a lazy loader replaces: an empty or inline tiny image. */
static int placeholder(const char *src)
{
    return !*src || !strncmp(src, "data:", 5) || strstr(src, "blank.gif") || strstr(src, "spacer.gif") ||
           strstr(src, "placeholder") || strstr(src, "lazy");
}

/* An attribute's value without the spaces around it. */
static const char *trimmed(const char *value, size_t *length)
{
    while (isspace((unsigned char)*value)) value++;
    size_t n = strlen(value);
    while (n && isspace((unsigned char)value[n - 1])) n--;
    *length = n;
    return value;
}

const char *picture_source(const browser_dom *dom, int img, int width, size_t *length, float *density)
{
    const dom_node *n = &dom->nodes[img];
    const char *chosen;
    *length = 0; *density = 1;
    if (n->parent >= 0 && has_tag(&dom->nodes[n->parent], "picture")) {
        for (int c = dom->nodes[n->parent].first; c >= 0 && c != img; c = dom->nodes[c].next) {
            const dom_node *s = &dom->nodes[c];
            if (!has_tag(s, "source") || !decodable_type(dom_attr(s, "type"))) continue;
            const char *media = dom_attr(s, "media");
            if (*media && !css_media_matches(media)) continue;
            const char *set = *dom_attr(s, "srcset") ? dom_attr(s, "srcset") : dom_attr(s, "data-srcset");
            if (*set && (chosen = candidate(set, width, length, density)) != NULL) return chosen;
        }
    }
    static const char *lazy[] = {"data-src", "data-lazy-src", "data-original", "data-lazy", "data-url"};
    static const char *lazy_sets[] = {"data-srcset", "data-lazy-srcset"};
    size_t m;
    const char *src = trimmed(dom_attr(n, "src"), &m);
    if (placeholder(src))
        for (size_t i = 0; i < sizeof(lazy) / sizeof(*lazy); i++)
            if (*dom_attr(n, lazy[i])) { src = trimmed(dom_attr(n, lazy[i]), &m); break; }
    int usable = m && !undecodable(src, m) && (strncmp(src, "data:", 5) || m <= PICTURE_DATA_MAX);
    const char *set = dom_attr(n, "srcset");
    for (size_t i = 0; i < sizeof(lazy_sets) / sizeof(*lazy_sets) && !*set; i++) set = dom_attr(n, lazy_sets[i]);
    if (*set && (chosen = candidate(set, width, length, density)) != NULL) {
        /* with only "2x" and up, src is the 1x picture */
        if (*density <= 1 || !usable || placeholder(src)) return chosen;
        *density = 1;
    }
    if (!usable) { *length = 0; return NULL; }
    *length = m;
    return src;
}

/* ---- the table ---- */

picture_table *picture_table_new(size_t budget, int nodes)
{
    picture_table *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->budget = budget;
    if (nodes > 0) {
        t->of_node = malloc((size_t)nodes * sizeof(*t->of_node));
        if (!t->of_node) { free(t); return NULL; }
        for (int i = 0; i < nodes; i++) t->of_node[i] = -1;
        t->nodes = nodes;
    }
    return t;
}

void picture_table_free(picture_table *t)
{
    if (!t) return;
    for (int i = 0; i < t->count; i++) { free(t->entries[i].url); picture_free(&t->entries[i].image); }
    free(t->of_node);
    free(t);
}

int picture_of(const picture_table *t, int node)
{
    return t && node >= 0 && node < t->nodes ? t->of_node[node] : -1;
}

static int same_source(const dom_node *a, const dom_node *b)
{
    static const char *names[] = {"src", "srcset", "data-src", "data-srcset", "data-lazy-src", "sizes"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) if (strcmp(dom_attr(a, names[i]), dom_attr(b, names[i]))) return 0;
    return 1;
}
void picture_table_rebind(picture_table *t, const browser_dom *old, const browser_dom *dom)
{
    if (!t || !t->of_node || !dom) return;
    int *at = old ? malloc((size_t)t->nodes * sizeof(int)) : NULL;
    if (at) {
        for (int i = 0; i < t->nodes; i++) at[i] = -1;
        for (int i = 0; i < old->count; i++) {
            int key = old->nodes[i].source_id;
            if (key >= 0 && key < t->nodes) at[key] = i;
        }
    }
    for (int i = 0; i < dom->count; i++) {
        const dom_node *n = &dom->nodes[i];
        int key = n->source_id;
        if (key < 0 || key >= t->nodes || t->of_node[key] < 0) continue;
        int was = at ? at[key] : -1;
        if (was < 0 || strcmp(old->nodes[was].tag, n->tag) || !same_source(&old->nodes[was], n)) t->of_node[key] = -1;
    }
    free(at);
}
int picture_add(picture_table *t, int node, const char *url, int want_w, int want_h, int sized, int top, float density)
{
    int index = picture_of(t, node);
    for (int i = 0; i < t->count && index < 0; i++) if (!strcmp(t->entries[i].url, url)) index = i;
    if (index >= 0) {
        picture_entry *e = &t->entries[index];
        /* 0 is any size: it stays */
        e->want_w = e->want_w && want_w ? (want_w > e->want_w ? want_w : e->want_w) : 0;
        e->want_h = e->want_h && want_h ? (want_h > e->want_h ? want_h : e->want_h) : 0;
        if (top < e->top) e->top = top;
        if (sized != e->sized) e->sized = PICTURE_FIT_BOX;
    } else {
        if (t->count >= PICTURES_MAX) return -1;
        picture_entry *e = &t->entries[t->count];
        memset(e, 0, sizeof(*e));
        e->url = strdup(url);
        if (!e->url) return -1;
        e->want_w = want_w; e->want_h = want_h; e->sized = sized; e->top = top;
        e->density = density < 0.5f ? 0.5f : density > 4 ? 4 : density;
        e->state = PICTURE_WAITING;
        index = t->count;
        BARRIER();
        t->count = index + 1;     /* published last: the entry is ready to be read */
    }
    if (node >= 0 && node < t->nodes) t->of_node[node] = (short)index;
    return index;
}

int picture_known(const picture_table *t, int node, int *w, int *h)
{
    int i = picture_of(t, node);
    if (i < 0 || t->entries[i].natural_w <= 0) return 0;
    const picture_entry *e = &t->entries[i];
    *w = (int)(e->natural_w * PICTURE_SCALE / e->density + 0.5f);
    *h = (int)(e->natural_h * PICTURE_SCALE / e->density + 0.5f);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    return 1;
}

int picture_next(const picture_table *t, int top, int bottom)
{
    int best = -1, distance = 0;
    for (int i = 0; i < t->count; i++) {
        const picture_entry *e = &t->entries[i];
        if (e->state != PICTURE_WAITING) continue;
        int d = e->top < top ? top - e->top : e->top >= bottom ? e->top - bottom + 1 : 0;
        /* what's below the screen comes before what's above it */
        if (e->top < top) d *= 2;
        if (best < 0 || d < distance) { best = i; distance = d; }
    }
    return best;
}

int picture_load(picture_table *t, int i, const unsigned char *data, size_t length)
{
    picture_entry *e = &t->entries[i];
    int w, h, tw, th;
    if (picture_size(data, length, &w, &h) < 0) return -1;
    int max_w = e->want_w, max_h = e->want_h;
    if (e->sized == PICTURE_FIT_OWN) {
        /* shown at its own size, so never decoded larger than that */
        int own_w = (int)(w * PICTURE_SCALE / e->density + 0.99f), own_h = (int)(h * PICTURE_SCALE / e->density + 0.99f);
        if (!max_w || own_w < max_w) max_w = own_w;
        if (!max_h || own_h < max_h) max_h = own_h;
    }
    fit(w, h, max_w, max_h, e->sized == PICTURE_FIT_STRETCH, &tw, &th);
    /* 2 bytes a pixel for JPEGs and PNGs without alpha, 4 for what may have it */
    int opaque = is_jpeg(data, length) || (is_png(data, length) && (data[25] == 0 || data[25] == 2));
    size_t left = t->budget > t->bytes ? t->budget - t->bytes : 0, need = (size_t)((tw + 8) & ~7) * (th + 1) * (opaque ? 2 : 4);
    if (need > left) {
        /* what's left of the budget, at a lower resolution */
        if (left < need / 16) return -1;
        float k = sqrtf((float)left / (float)need) * 0.95f;
        tw = (int)(tw * k); th = (int)(th * k);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    picture_image image;
    if (picture_decode(data, length, tw, th, 1, &image, &w, &h) < 0) return -1;
    e->natural_w = w; e->natural_h = h;
    e->image = image;
    t->bytes += picture_bytes(&image);
    if (e->sized != PICTURE_FIT_STRETCH || e->relayout) t->learned++;
    return 0;
}
