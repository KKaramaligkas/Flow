/* Pictures: decoding JPEG, PNG and GIF into shrunk 16-bit images, choosing
   an <img>'s source, and the page's table. The test pictures are made here
   with the libraries' own encoders. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <png.h>
#include <gif_lib.h>
#include "../src/document.h"
#include "../src/picture.h"
static int checks, failures;
#define CHECK(test) do{checks++;if(!(test)){failures++;fprintf(stderr,"%d: %s\n",__LINE__,#test);}}while(0)
#define SAME(a,b) CHECK(strcmp((a),(b))==0)

typedef struct { unsigned char *data; size_t size, capacity; } buffer;
static void append(buffer *b, const void *p, size_t n)
{
    if (b->size + n > b->capacity) {
        b->capacity = (b->size + n) * 2 + 64;
        b->data = realloc(b->data, b->capacity);
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

/* Test pictures: the left half red, the right half blue (or, with alpha,
   green and fully transparent). */
static void halves(int x, int w, int alpha, unsigned char *c)
{
    int left = x < w / 2;
    if (alpha) { c[0] = 0; c[1] = left ? 255 : 0; c[2] = 0; c[3] = left ? 255 : 0; }
    else { c[0] = left ? 255 : 0; c[1] = 0; c[2] = left ? 0 : 255; c[3] = 255; }
}

static unsigned char *make_jpeg(int w, int h, int progressive, int gray, size_t *size)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr e;
    unsigned char *out = NULL;
    unsigned long n = 0;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &n);
    c.image_width = (JDIMENSION)w; c.image_height = (JDIMENSION)h;
    c.input_components = gray ? 1 : 3; c.in_color_space = gray ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 95, TRUE);
    if (progressive) jpeg_simple_progression(&c);
    jpeg_start_compress(&c, TRUE);
    unsigned char *row = malloc((size_t)w * 3);
    while (c.next_scanline < c.image_height) {
        for (int x = 0; x < w; x++) {
            unsigned char px[4];
            halves(x, w, 0, px);
            if (gray) row[x] = 128; else memcpy(row + x * 3, px, 3);
        }
        JSAMPROW r = row;
        jpeg_write_scanlines(&c, &r, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    *size = n;
    return out;
}

static void png_write(png_structp png, png_bytep p, png_size_t n) { append(png_get_io_ptr(png), p, n); }
static void png_flush_nothing(png_structp png) { (void)png; }
/* type: PNG_COLOR_TYPE_RGB, _RGB_ALPHA, _GRAY (16 bits, mid grey), _PALETTE
   (red, and a transparent second colour on the right) or _GRAY_ALPHA. */
static unsigned char *make_png(int w, int h, int type, int interlace, size_t *size)
{
    buffer b = {0};
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_set_write_fn(png, &b, png_write, png_flush_nothing);
    int depth = type == PNG_COLOR_TYPE_GRAY ? 16 : 8;
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, depth, type, interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (type == PNG_COLOR_TYPE_PALETTE) {
        png_color colors[2] = {{255, 0, 0}, {0, 0, 255}};
        png_byte alpha[2] = {255, 0};
        png_set_PLTE(png, info, colors, 2);
        png_set_tRNS(png, info, alpha, 2, NULL);
    }
    png_write_info(png, info);
    int channels = type == PNG_COLOR_TYPE_RGB ? 3 : type == PNG_COLOR_TYPE_RGB_ALPHA ? 4 : type == PNG_COLOR_TYPE_GRAY_ALPHA ? 2 : type == PNG_COLOR_TYPE_GRAY ? 2 : 1;
    unsigned char *rows = malloc((size_t)w * h * channels);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *o = rows + ((size_t)y * w + x) * channels, px[4];
            halves(x, w, type == PNG_COLOR_TYPE_RGB_ALPHA || type == PNG_COLOR_TYPE_GRAY_ALPHA, px);
            if (type == PNG_COLOR_TYPE_RGB) memcpy(o, px, 3);
            else if (type == PNG_COLOR_TYPE_RGB_ALPHA) memcpy(o, px, 4);
            else if (type == PNG_COLOR_TYPE_GRAY) { o[0] = 0x80; o[1] = 0x80; }
            else if (type == PNG_COLOR_TYPE_GRAY_ALPHA) { o[0] = 200; o[1] = px[3]; }
            else o[0] = x < w / 2 ? 0 : 1;
        }
    png_bytep *pointers = malloc(sizeof(png_bytep) * h);
    for (int y = 0; y < h; y++) pointers[y] = rows + (size_t)y * w * channels;
    png_write_image(png, pointers);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    free(pointers); free(rows);
    *size = b.size;
    return b.data;
}

/* A solid colour, fast to make at any size. */
static unsigned char *make_solid_png(int w, int h, unsigned char r, unsigned char g, unsigned char bl, size_t *size)
{
    buffer b = {0};
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_set_write_fn(png, &b, png_write, png_flush_nothing);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    unsigned char *row = malloc((size_t)w * 3);
    for (int x = 0; x < w; x++) { row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = bl; }
    for (int y = 0; y < h; y++) png_write_row(png, row);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    free(row);
    *size = b.size;
    return b.data;
}

static int gif_write(GifFileType *g, const GifByteType *p, int n) { append(g->UserData, p, (size_t)n); return n; }
/* Colours red, blue, green, white; with `transparent`, white is clear. The
   frame (fw x fh at fx, fy) is the left half red, the right half blue, with
   a white column in the middle. */
static unsigned char *make_gif(int w, int h, int fx, int fy, int fw, int fh, int interlace, int transparent, size_t *size)
{
    buffer b = {0};
    int error;
    GifFileType *g = EGifOpen(&b, gif_write, &error);
    GifColorType colors[4] = {{255, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 255, 255}};
    ColorMapObject *map = GifMakeMapObject(4, colors);
    EGifSetGifVersion(g, true);
    EGifPutScreenDesc(g, w, h, 2, 0, map);
    if (transparent) {
        GraphicsControlBlock gcb = {0, false, 0, 3};
        GifByteType extension[4];
        EGifGCBToExtension(&gcb, extension);
        EGifPutExtension(g, GRAPHICS_EXT_FUNC_CODE, 4, extension);
    }
    EGifPutImageDesc(g, fx, fy, fw, fh, interlace, NULL);
    GifPixelType *line = malloc((size_t)fw);
    for (int x = 0; x < fw; x++) line[x] = x == fw / 2 ? 3 : x < fw / 2 ? 0 : 1;
    static const int start[] = {0, 4, 2, 1}, step[] = {8, 8, 4, 2};
    if (interlace) { for (int pass = 0; pass < 4; pass++) for (int y = start[pass]; y < fh; y += step[pass]) EGifPutLine(g, line, fw); }
    else for (int y = 0; y < fh; y++) EGifPutLine(g, line, fw);
    EGifCloseFile(g, &error);
    GifFreeMapObject(map);
    free(line);
    *size = b.size;
    return b.data;
}

static unsigned short pixel(const picture_image *im, int x, int y) { return ((const unsigned short *)im->pixels)[(size_t)y * im->stride + x]; }
/* 0xAABBGGRR */
static unsigned pixel32(const picture_image *im, int x, int y) { return ((const unsigned *)im->pixels)[(size_t)y * im->stride + x]; }
#define RED_5650 0x001F
#define BLUE_5650 0xF800

static void decoding(void)
{
    size_t n;
    int nw, nh;
    picture_image im;
    /* JPEG: shrunk while decoded, keeping its shape */
    unsigned char *jpeg = make_jpeg(64, 32, 0, 0, &n);
    CHECK(picture_size(jpeg, n, &nw, &nh) == 0 && nw == 64 && nh == 32);
    CHECK(picture_decode(jpeg, n, 16, 16, 0, &im, &nw, &nh) == 0);
    CHECK(nw == 64 && nh == 32 && im.w == 16 && im.h == 8 && im.format == PIXELS_5650);
    CHECK(im.stride % 8 == 0 && im.stride > im.w && ((size_t)im.pixels & 15) == 0);
    unsigned short left = pixel(&im, 1, 4), right = pixel(&im, 14, 4);
    CHECK((left & 0x1F) >= 28 && (left >> 11) <= 3);    /* red */
    CHECK((right >> 11) >= 28 && (right & 0x1F) <= 3);  /* blue */
    CHECK(pixel(&im, im.w, 3) == pixel(&im, im.w - 1, 3) && pixel(&im, 5, im.h) == pixel(&im, 5, im.h - 1));    /* the edges repeat */
    CHECK(picture_bytes(&im) == (size_t)im.stride * (im.h + 1) * 2);
    picture_free(&im);
    CHECK(!im.pixels && picture_bytes(&im) == 0);
    /* stretched to a box of another shape, each side on its own, never enlarged */
    CHECK(picture_decode(jpeg, n, 10, 30, 1, &im, &nw, &nh) == 0 && im.w == 10 && im.h == 30);
    picture_free(&im);
    CHECK(picture_decode(jpeg, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.w == 64 && im.h == 32);
    picture_free(&im);
    /* truncated: whatever libjpeg makes of it, nothing breaks */
    if (picture_decode(jpeg, n / 2, 64, 64, 0, &im, &nw, &nh) == 0) CHECK(im.w == 64 && im.h == 32);
    picture_free(&im);
    size_t baseline = picture_decode_memory(jpeg, n);
    free(jpeg);
    /* a big progressive JPEG: decoded at 1/8 and shrunk; its coefficients are counted */
    jpeg = make_jpeg(800, 600, 1, 0, &n);
    CHECK(picture_decode(jpeg, n, 100, 100, 0, &im, &nw, &nh) == 0 && nw == 800 && nh == 600 && im.w == 100 && im.h == 75);
    CHECK((pixel(&im, 10, 30) & 0x1F) >= 28 && (pixel(&im, 90, 30) >> 11) >= 28);
    picture_free(&im);
    CHECK(baseline > 0 && baseline < 1400000 && picture_decode_memory(jpeg, n) >= 1446400 + 256 * 1024);
    free(jpeg);
    /* greyscale */
    jpeg = make_jpeg(20, 10, 0, 1, &n);
    CHECK(picture_decode(jpeg, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.w == 20 && im.format == PIXELS_5650);
    CHECK(abs((pixel(&im, 3, 3) & 0x1F) - 16) <= 1 && abs(((pixel(&im, 3, 3) >> 5) & 0x3F) - 32) <= 1);
    picture_free(&im);
    free(jpeg);

    /* PNG: RGB, exact colours */
    unsigned char *png = make_png(64, 32, PNG_COLOR_TYPE_RGB, 0, &n);
    CHECK(picture_size(png, n, &nw, &nh) == 0 && nw == 64 && nh == 32);
    CHECK(picture_decode(png, n, 16, 8, 0, &im, &nw, &nh) == 0 && im.w == 16 && im.h == 8 && im.format == PIXELS_5650);
    CHECK(pixel(&im, 0, 0) == RED_5650 && pixel(&im, 7, 7) == RED_5650 && pixel(&im, 8, 0) == BLUE_5650 && pixel(&im, 15, 7) == BLUE_5650);
    picture_free(&im);
    /* truncated or broken: refused */
    CHECK(picture_decode(png, n - 20, 16, 8, 0, &im, &nw, &nh) < 0 && !im.pixels);
    png[40] ^= 0x55;
    CHECK(picture_decode(png, n, 16, 8, 0, &im, &nw, &nh) < 0 && !im.pixels);
    free(png);
    /* interlaced, at its own size */
    png = make_png(33, 17, PNG_COLOR_TYPE_RGB, 1, &n);
    CHECK(picture_decode(png, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.w == 33 && im.h == 17);
    CHECK(pixel(&im, 0, 16) == RED_5650 && pixel(&im, 15, 9) == RED_5650 && pixel(&im, 16, 3) == BLUE_5650 && pixel(&im, 32, 16) == BLUE_5650);
    picture_free(&im);
    free(png);
    /* with alpha: 32 bits, and the clear side takes the colour beside it */
    png = make_png(32, 32, PNG_COLOR_TYPE_RGB_ALPHA, 0, &n);
    CHECK(picture_decode(png, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.format == PIXELS_8888);
    CHECK(pixel32(&im, 0, 0) == 0xFF00FF00 && pixel32(&im, 15, 5) == 0xFF00FF00);
    CHECK(pixel32(&im, 16, 5) == 0x0000FF00 && pixel32(&im, 31, 5) == 0 && pixel32(&im, 32, 5) == 0 && pixel32(&im, 3, 32) == 0xFF00FF00);
    CHECK(picture_bytes(&im) == (size_t)im.stride * 33 * 4);
    picture_free(&im);
    free(png);
    /* a palette with a clear colour */
    png = make_png(8, 4, PNG_COLOR_TYPE_PALETTE, 0, &n);
    CHECK(picture_decode(png, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.format == PIXELS_8888);
    CHECK(pixel32(&im, 0, 0) == 0xFF0000FF && (pixel32(&im, 7, 0) >> 24) == 0);
    picture_free(&im);
    free(png);
    /* 16-bit grey: mid grey */
    png = make_png(10, 10, PNG_COLOR_TYPE_GRAY, 0, &n);
    CHECK(picture_decode(png, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.format == PIXELS_5650 && pixel(&im, 4, 4) == 0x8410);
    picture_free(&im);
    free(png);
    /* grey with alpha */
    png = make_png(10, 4, PNG_COLOR_TYPE_GRAY_ALPHA, 0, &n);
    CHECK(picture_decode(png, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.format == PIXELS_8888 && pixel32(&im, 0, 0) == 0xFFC8C8C8 && (pixel32(&im, 9, 0) >> 24) == 0);
    picture_free(&im);
    free(png);
    /* a huge picture shrunk to a dot: decoded bigger rather than overflow the sums */
    png = make_solid_png(2048, 2048, 255, 255, 255, &n);
    CHECK(picture_decode(png, n, 1, 1, 0, &im, &nw, &nh) == 0 && im.w == 16 && im.h == 16 && pixel(&im, 8, 8) == 0xFFFF);
    picture_free(&im);
    free(png);
    png = make_solid_png(4000, 10, 0, 255, 0, &n);
    CHECK(picture_decode(png, n, 4, 4, 0, &im, &nw, &nh) == 0 && im.w == 32 && im.h == 1 && pixel(&im, 5, 0) == 0x07E0);
    picture_free(&im);
    free(png);

    /* GIF: the first frame, with its clear colour, interlaced or not */
    unsigned char *gif = make_gif(21, 10, 0, 0, 21, 10, 0, 0, &n);
    CHECK(picture_size(gif, n, &nw, &nh) == 0 && nw == 21 && nh == 10);
    CHECK(picture_decode(gif, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.w == 21 && im.h == 10 && im.format == PIXELS_5650);
    CHECK(pixel(&im, 0, 0) == RED_5650 && pixel(&im, 10, 9) == 0xFFFF && pixel(&im, 20, 9) == BLUE_5650);
    picture_free(&im);
    free(gif);
    gif = make_gif(21, 19, 0, 0, 21, 19, 1, 1, &n);
    CHECK(picture_decode(gif, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.format == PIXELS_8888);
    int clear = 1;
    for (int y = 0; y < 19; y++) clear &= (pixel32(&im, 10, y) >> 24) == 0 && pixel32(&im, 0, y) == 0xFF0000FF && pixel32(&im, 20, y) == 0xFFFF0000;
    CHECK(clear);
    picture_free(&im);
    free(gif);
    /* a frame smaller than the screen: clear around it */
    gif = make_gif(20, 20, 5, 5, 9, 9, 0, 0, &n);
    CHECK(picture_decode(gif, n, 0, 0, 0, &im, &nw, &nh) == 0 && im.w == 20 && im.format == PIXELS_8888);
    CHECK((pixel32(&im, 0, 0) >> 24) == 0 && pixel32(&im, 5, 5) == 0xFF0000FF && pixel32(&im, 13, 13) == 0xFFFF0000 && (pixel32(&im, 19, 19) >> 24) == 0);
    picture_free(&im);
    free(gif);

    /* not a picture, or a header with an impossible size */
    CHECK(picture_decode((const unsigned char *)"hello world, this is not a picture at all", 41, 0, 0, 0, &im, &nw, &nh) < 0);
    CHECK(picture_size((const unsigned char *)"\xFF\xD8\xFF", 3, &nw, &nh) < 0);
    png = make_png(8, 4, PNG_COLOR_TYPE_RGB, 0, &n);
    png[16] = 0; png[17] = 1; png[18] = 0x86; png[19] = 0xA0;     /* 100000 wide */
    CHECK(picture_size(png, n, &nw, &nh) < 0 && picture_decode(png, n, 0, 0, 0, &im, &nw, &nh) < 0);
    free(png);

    /* damaged copies decode or fail, but nothing breaks (the sanitizers watch) */
    unsigned char *samples[4];
    size_t sizes[4];
    samples[0] = make_jpeg(40, 30, 0, 0, &sizes[0]);
    samples[1] = make_jpeg(40, 30, 1, 0, &sizes[1]);
    samples[2] = make_png(40, 30, PNG_COLOR_TYPE_RGB_ALPHA, 1, &sizes[2]);
    samples[3] = make_gif(40, 30, 2, 3, 30, 20, 1, 1, &sizes[3]);
    unsigned seed = 12345;
    int decoded = 0;
    for (int s = 0; s < 4; s++) {
        unsigned char *copy = malloc(sizes[s]);
        for (int round = 0; round < 400; round++) {
            memcpy(copy, samples[s], sizes[s]);
            for (int k = 0; k < 1 + round % 4; k++) {
                seed = seed * 1103515245 + 12345;
                copy[(seed >> 8) % sizes[s]] ^= (unsigned char)(1 << ((seed >> 4) & 7));
            }
            size_t length = round % 5 == 4 ? (seed >> 3) % sizes[s] : sizes[s];
            if (picture_decode(copy, length, 30, 30, round & 1, &im, &nw, &nh) == 0) {
                decoded++;
                CHECK(im.w >= 1 && im.h >= 1 && im.w <= PICTURE_SIDE_MAX && im.h <= PICTURE_SIDE_MAX && im.pixels);
                picture_free(&im);
            }
        }
        free(copy);
        free(samples[s]);
    }
    CHECK(decoded > 100);
}

static void data_uris(void)
{
    size_t n, m;
    unsigned char *png = make_png(8, 4, PNG_COLOR_TYPE_RGB, 0, &n);
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *uri = malloc(n * 2 + 64), *p;
    p = uri + sprintf(uri, "data:image/png;base64,");
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = png[i] << 16 | (i + 1 < n ? png[i + 1] << 8 : 0) | (i + 2 < n ? png[i + 2] : 0);
        *p++ = alphabet[v >> 18]; *p++ = alphabet[(v >> 12) & 63];
        *p++ = i + 1 < n ? alphabet[(v >> 6) & 63] : '=';
        *p++ = i + 2 < n ? alphabet[v & 63] : '=';
        if (i % 30 == 0) *p++ = '\n';
    }
    *p = 0;
    unsigned char *data = picture_data(uri, &m);
    CHECK(data && m == n && !memcmp(data, png, n));
    free(data);
    /* escaped padding */
    char *escaped = malloc(strlen(uri) * 3 + 1);
    strcpy(escaped, uri);
    for (char *q = escaped; (q = strchr(q, '=')) != NULL;) { memmove(q + 3, q + 1, strlen(q)); memcpy(q, "%3D", 3); q += 3; }
    data = picture_data(escaped, &m);
    CHECK(data && m == n && !memcmp(data, png, n));
    free(data);
    free(escaped);
    CHECK(!picture_data("data:image/svg+xml;base64,PHN2Zz4=", &m));
    CHECK(!picture_data("data:image/png,rawbytes", &m));
    CHECK(!picture_data("data:image/png;base64,ab*cd", &m));
    CHECK(!picture_data("https://example.org/a.png", &m));
    free(uri);
    free(png);
}

typedef struct { browser_document doc; } page;
static page *load(const char *html)
{
    page *p = calloc(1, sizeof(*p));
    char err[256];
    CHECK(browser_document_parse(&p->doc, html, strlen(html), "https://example.org/", "text/html", err, sizeof(err)) == 0);
    return p;
}
static void drop(page *p) { browser_document_free(&p->doc); free(p); }
static int nth_img(const browser_dom *dom, int k)
{
    for (int i = 0; i < dom->count; i++) if (!strcmp(dom->nodes[i].tag, "img") && k-- == 0) return i;
    return -1;
}
/* The source of the k-th <img> for a box `width` wide, as a string. */
static const char *source(page *p, int k, int width, float *density)
{
    static char out[512];
    size_t n;
    float d;
    int img = nth_img(p->doc.dom, k);
    const char *s = img >= 0 ? picture_source(p->doc.dom, img, width, &n, &d) : NULL;
    if (density) *density = d;
    if (!s) return "(none)";
    snprintf(out, sizeof(out), "%.*s", (int)n, s);
    return out;
}

static void sources(void)
{
    float d;
    page *p = load("<img src=a.jpg><img src=' a.jpg ' srcset='b.jpg 100w, c.jpg 300w,d.jpg 600w'>"
                   "<img src=a.jpg srcset='a2.jpg 2x'><img srcset='a1.jpg 1x, a2.jpg 2x'><img srcset='a2.jpg 2x, a3.jpg 3x'>"
                   "<img srcset='https://r.example/w_100,h_50/a.jpg 100w, https://r.example/w_400,h_200/a.jpg 400w'>");
    SAME(source(p, 0, 100, &d), "a.jpg"); CHECK(d == 1);
    SAME(source(p, 1, 200, NULL), "c.jpg");
    SAME(source(p, 1, 50, NULL), "b.jpg");
    SAME(source(p, 1, 300, NULL), "c.jpg");
    SAME(source(p, 1, 1000, NULL), "d.jpg");     /* none wide enough: the widest */
    SAME(source(p, 2, 100, &d), "a.jpg"); CHECK(d == 1);    /* only 2x: src is 1x */
    SAME(source(p, 3, 100, &d), "a1.jpg"); CHECK(d == 1);
    SAME(source(p, 4, 100, &d), "a2.jpg"); CHECK(d == 2);
    SAME(source(p, 5, 300, NULL), "https://r.example/w_400,h_200/a.jpg");   /* commas inside an address */
    drop(p);
    /* <picture>: a <source> in a format this decodes, whose media match */
    p = load("<picture><source srcset=x.webp type=image/webp><source srcset=y.jpg media='(max-width: 300px)'>"
             "<source srcset='z1.jpg 1x, z2.jpg 2x'><img src=fallback.jpg></picture>"
             "<picture><source srcset=x.avif type=image/avif><img src=only.png></picture>"
             "<picture><source srcset='w.avif'><source srcset='v.jpg' type=image/jpeg><img src=f.jpg></picture>");
    SAME(source(p, 0, 100, NULL), "z1.jpg");
    SAME(source(p, 1, 100, NULL), "only.png");
    SAME(source(p, 2, 100, NULL), "v.jpg");     /* .avif by its name */
    drop(p);
    /* lazy loaders */
    p = load("<img src='data:image/gif;base64,R0lGODlhAQABAAAAACw=' data-src=real.jpg>"
             "<img class=lazyload data-src=' r.jpg'>"
             "<img src=placeholder.png data-srcset='s1.jpg 100w, s2.jpg 400w'>"
             "<img src=/img/blank.gif data-original=o.jpg>"
             "<img src='data:image/png;base64,iVBORw0KGgo=' alt=inline>"
             "<img src=lazy-dog.jpg>");
    SAME(source(p, 0, 100, NULL), "real.jpg");
    SAME(source(p, 1, 100, NULL), "r.jpg");
    SAME(source(p, 2, 300, NULL), "s2.jpg");
    SAME(source(p, 3, 100, NULL), "o.jpg");
    SAME(source(p, 4, 100, NULL), "data:image/png;base64,iVBORw0KGgo=");    /* a real inline picture */
    SAME(source(p, 5, 100, NULL), "lazy-dog.jpg");
    drop(p);
    /* formats this can't show: no source */
    p = load("<img src=logo.svg><img src='data:image/svg+xml;base64,PHN2Zz4='><img src='pic.webp?x=1'>"
             "<img src='pic.jpg?format=webp'><img><img src='  '><img srcset='a.svg 1x' src=b.svg>");
    SAME(source(p, 0, 100, NULL), "(none)");
    SAME(source(p, 1, 100, NULL), "(none)");
    SAME(source(p, 2, 100, NULL), "(none)");
    SAME(source(p, 3, 100, NULL), "pic.jpg?format=webp");
    SAME(source(p, 4, 100, NULL), "(none)");
    SAME(source(p, 5, 100, NULL), "(none)");
    SAME(source(p, 6, 100, NULL), "(none)");
    drop(p);
}

static void table(void)
{
    picture_table *t = picture_table_new(1024 * 1024, 10);
    CHECK(t && t->count == 0 && picture_of(t, 3) < 0 && picture_of(t, 99) < 0);
    CHECK(picture_add(t, 3, "https://x/a.jpg", 100, 50, PICTURE_FIT_STRETCH, 500, 1) == 0);
    CHECK(picture_add(t, 4, "https://x/a.jpg", 200, 20, PICTURE_FIT_STRETCH, 100, 1) == 0);    /* the same picture */
    CHECK(t->entries[0].want_w == 200 && t->entries[0].want_h == 50 && t->entries[0].top == 100);
    CHECK(picture_add(t, 5, "https://x/b.jpg", 0, 80, PICTURE_FIT_BOX, 900, 2) == 1 && t->count == 2);
    CHECK(picture_of(t, 4) == 0 && picture_of(t, 5) == 1 && t->entries[1].density == 2);
    /* a node keeps its picture when laid out again */
    CHECK(picture_add(t, 5, "https://x/other.jpg", 0, 90, PICTURE_FIT_BOX, 900, 1) == 1 && t->count == 2 && t->entries[1].want_h == 90);
    CHECK(picture_add(t, 6, "https://x/a.jpg", 0, 40, PICTURE_FIT_BOX, 100, 1) == 0 && t->entries[0].want_w == 0 && t->entries[0].sized == PICTURE_FIT_BOX);
    /* nearest the screen first, below before above */
    CHECK(picture_next(t, 0, 272) == 0);
    t->entries[0].state = PICTURE_READY;
    CHECK(picture_next(t, 0, 272) == 1);
    CHECK(picture_add(t, -1, "https://x/c.jpg", 10, 10, PICTURE_FIT_STRETCH, 300, 1) == 2);
    CHECK(picture_next(t, 450, 722) == 1);      /* 900 is 179 below; 300 is 150 above, counted twice */
    CHECK(picture_next(t, 400, 672) == 2);      /* 229 below; 100 above */
    t->entries[1].state = PICTURE_FAILED;
    CHECK(picture_next(t, 450, 722) == 2);
    t->entries[2].state = PICTURE_READY;
    CHECK(picture_next(t, 0, 272) < 0);
    /* full */
    char url[64];
    for (int i = t->count; i < PICTURES_MAX; i++) { snprintf(url, sizeof(url), "https://x/%d.jpg", i); CHECK(picture_add(t, -1, url, 1, 1, 0, 0, 1) == i); }
    CHECK(picture_add(t, -1, "https://x/more.jpg", 1, 1, 0, 0, 1) < 0 && t->count == PICTURES_MAX);
    CHECK(picture_add(t, 7, "https://x/b.jpg", 1, 1, 0, 0, 1) == 1);    /* known ones still are */
    picture_table_free(t);
    picture_table_free(NULL);

    /* loading: the size each kind of box needs */
    size_t n;
    unsigned char *png = make_png(100, 50, PNG_COLOR_TYPE_RGB, 0, &n);
    int w, h;
    t = picture_table_new(1024 * 1024, 10);
    picture_add(t, 0, "own", 400, 0, PICTURE_FIT_OWN, 0, 1);
    picture_add(t, 1, "dense", 400, 0, PICTURE_FIT_OWN, 0, 2);
    picture_add(t, 2, "wide", 40, 0, PICTURE_FIT_BOX, 0, 1);
    picture_add(t, 3, "box", 30, 30, PICTURE_FIT_STRETCH, 0, 1);
    picture_add(t, 4, "narrow", 20, 0, PICTURE_FIT_OWN, 0, 1);
    CHECK(!picture_known(t, 0, &w, &h));
    for (int i = 0; i < 5; i++) CHECK(picture_load(t, i, png, n) == 0 && t->entries[i].natural_w == 100 && t->entries[i].natural_h == 50);
    CHECK(t->entries[0].image.w == 60 && t->entries[0].image.h == 30);     /* its own size at 0.6 */
    CHECK(t->entries[1].image.w == 30 && t->entries[1].image.h == 15);     /* at 2x */
    CHECK(t->entries[2].image.w == 40 && t->entries[2].image.h == 20);
    CHECK(t->entries[3].image.w == 30 && t->entries[3].image.h == 30);
    CHECK(t->entries[4].image.w == 20 && t->entries[4].image.h == 10);
    CHECK(t->learned == 4);
    CHECK(picture_known(t, 0, &w, &h) && w == 60 && h == 30);
    CHECK(picture_known(t, 1, &w, &h) && w == 30 && h == 15);
    CHECK(!picture_known(t, 9, &w, &h));
    size_t used = 0;
    for (int i = 0; i < 5; i++) used += picture_bytes(&t->entries[i].image);
    CHECK(t->bytes == used);
    picture_add(t, 5, "bad", 40, 0, PICTURE_FIT_BOX, 0, 1);
    CHECK(picture_load(t, 5, (const unsigned char *)"not a picture at all", 20) < 0 && !t->entries[5].image.pixels && t->bytes == used);
    picture_table_free(t);
    /* past the budget: smaller, then refused */
    t = picture_table_new(4000, 0);
    picture_add(t, -1, "a", 64, 32, PICTURE_FIT_STRETCH, 0, 1);
    picture_add(t, -1, "b", 64, 32, PICTURE_FIT_STRETCH, 0, 1);
    picture_add(t, -1, "c", 64, 32, PICTURE_FIT_STRETCH, 0, 1);
    CHECK(picture_load(t, 0, png, n) == 0 && t->entries[0].image.w < 64 && t->entries[0].image.w >= 40 && t->bytes <= 4000);
    CHECK(picture_load(t, 1, png, n) == 0 && t->entries[1].image.w < 30 && t->bytes <= 4000);
    CHECK(picture_load(t, 2, png, n) < 0 && !t->entries[2].image.pixels && t->entries[2].natural_w == 0);
    picture_table_free(t);
    free(png);
}

int main(void)
{
    decoding();
    data_uris();
    sources();
    table();
    printf("picture: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
