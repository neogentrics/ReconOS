/*
 * Writing PNG files. See include/recon_png.h.
 *
 * A PNG is a signature, then a sequence of chunks, each of which is a length,
 * a four-letter type, its data, and a CRC of the type and data together. The
 * three that matter here are IHDR (how big, and what the pixels are), IDAT
 * (the pixels, deflated), and IEND (that was all).
 *
 * The pixel data is not deflated as-is. Each row is prefixed with a filter
 * byte saying how that row was transformed before compression, which exists
 * because a row usually resembles the one above it and saying how they differ
 * compresses better than saying what they are. Filter 0 means "not
 * transformed", which compresses worse and cannot be got wrong; this writes
 * screenshots, where the saving would be real but the correctness matters
 * more.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "recon_fs.h"
#include "recon_png.h"

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_png_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* Big-endian, because that is the byte order a PNG is written in. */
static void put32(unsigned char *out, unsigned long value) {
    out[0] = (unsigned char)((value >> 24) & 0xFF);
    out[1] = (unsigned char)((value >> 16) & 0xFF);
    out[2] = (unsigned char)((value >> 8) & 0xFF);
    out[3] = (unsigned char)(value & 0xFF);
}

/*
 * One chunk, appended at `used`. Returns the new length.
 *
 * The CRC covers the type and the data but not the length, which is a quirk
 * of the format rather than a decision made here.
 */
static size_t put_chunk(unsigned char *out, size_t used, const char *type,
        const unsigned char *data, size_t length) {
    put32(out + used, (unsigned long)length);
    used += 4;

    memcpy(out + used, type, 4);
    size_t crc_start = used;
    used += 4;

    if (length > 0) {
        memcpy(out + used, data, length);
        used += length;
    }

    unsigned long crc = crc32(0, out + crc_start, (unsigned)(4 + length));
    put32(out + used, crc);
    return used + 4;
}

unsigned char *recon_png_encode(const unsigned int *pixels, int width,
        int height, bool with_alpha, size_t *size_out) {
    if (pixels == NULL || width <= 0 || height <= 0) {
        set_error("nothing to encode");
        return NULL;
    }
    /* A guard against an accidental multiply overflowing into a small
     * allocation, which is how an image size becomes a memory bug. */
    if (width > 16384 || height > 16384) {
        set_error("image too large");
        return NULL;
    }

    int channels = with_alpha ? 4 : 3;
    size_t stride = (size_t)width * channels + 1;   /* +1 for the filter byte */
    size_t raw_size = stride * (size_t)height;

    unsigned char *raw = malloc(raw_size);
    if (raw == NULL) {
        set_error("out of memory");
        return NULL;
    }

    for (int y = 0; y < height; y++) {
        unsigned char *row = raw + (size_t)y * stride;
        *row++ = 0;   /* filter: none */

        const unsigned int *source = pixels + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            unsigned int pixel = source[x];
            *row++ = (unsigned char)((pixel >> 16) & 0xFF);   /* red */
            *row++ = (unsigned char)((pixel >> 8) & 0xFF);    /* green */
            *row++ = (unsigned char)(pixel & 0xFF);           /* blue */
            if (with_alpha) {
                *row++ = (unsigned char)((pixel >> 24) & 0xFF);
            }
        }
    }

    uLongf packed_size = compressBound((uLong)raw_size);
    unsigned char *packed = malloc(packed_size);
    if (packed == NULL) {
        free(raw);
        set_error("out of memory");
        return NULL;
    }

    if (compress2(packed, &packed_size, raw, (uLong)raw_size, 6) != Z_OK) {
        free(raw);
        free(packed);
        set_error("could not compress the image");
        return NULL;
    }
    free(raw);

    /* Signature, IHDR, IDAT, IEND, plus each chunk's twelve bytes of
     * bookkeeping. */
    size_t total = 8 + (12 + 13) + (12 + packed_size) + 12;
    unsigned char *out = malloc(total);
    if (out == NULL) {
        free(packed);
        set_error("out of memory");
        return NULL;
    }

    static const unsigned char SIGNATURE[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'
    };
    memcpy(out, SIGNATURE, sizeof(SIGNATURE));
    size_t used = sizeof(SIGNATURE);

    unsigned char header[13];
    put32(header, (unsigned long)width);
    put32(header + 4, (unsigned long)height);
    header[8] = 8;                              /* bits per channel */
    header[9] = with_alpha ? 6 : 2;             /* 2 = RGB, 6 = RGBA */
    header[10] = 0;                             /* deflate */
    header[11] = 0;                             /* the only filter method */
    header[12] = 0;                             /* not interlaced */

    used = put_chunk(out, used, "IHDR", header, sizeof(header));
    used = put_chunk(out, used, "IDAT", packed, packed_size);
    used = put_chunk(out, used, "IEND", NULL, 0);

    free(packed);

    if (size_out != NULL) {
        *size_out = used;
    }
    return out;
}

bool recon_png_write(const char *path, const unsigned int *pixels, int width,
        int height, bool with_alpha) {
    size_t size = 0;
    unsigned char *data = recon_png_encode(pixels, width, height, with_alpha,
        &size);
    if (data == NULL) {
        return false;
    }

    bool ok = recon_fs_write("/", path, (const char *)data, size);
    if (!ok) {
        set_error("%s", recon_fs_last_error());
    }
    free(data);
    return ok;
}
