/*
 * ICO decoding. See include/recon_ico.h.
 *
 * An ICO is a container: a directory of images at different sizes, each stored
 * either as PNG or as a Windows DIB. The PNG case is handed to stb_image; the
 * DIB case is decoded here, because it is the older form and the one nearly
 * every icon from the Windows 95 era actually uses.
 *
 * A DIB inside an icon differs from a bitmap file in two ways worth knowing:
 * its declared height is twice the real height, because a transparency mask
 * follows the image, and its rows run bottom to top.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "recon_ico.h"
#include "stb_image.h"

/* --- Reading little-endian fields --- */

static uint16_t read_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- DIB --- */

#define DIB_HEADER_SIZE 40

/*
 * Decode the DIB form into RGBA.
 *
 * Returns NULL for anything unsupported rather than guessing, so a format we
 * cannot read fails visibly instead of producing garbage.
 */
static unsigned char *decode_dib(const unsigned char *data, size_t size,
        int *width_out, int *height_out) {
    if (size < DIB_HEADER_SIZE) {
        return NULL;
    }

    uint32_t header_size = read_u32(data);
    if (header_size < DIB_HEADER_SIZE) {
        return NULL;
    }

    int width = (int)read_u32(data + 4);
    int declared_height = (int)read_u32(data + 8);
    uint16_t bpp = read_u16(data + 14);
    uint32_t compression = read_u32(data + 16);
    uint32_t palette_count = read_u32(data + 32);

    /* Only uncompressed images; the compressed forms are vanishingly rare in
     * icons and would be a decoder of their own. */
    if (compression != 0) {
        return NULL;
    }

    /* The declared height covers the image and the mask that follows it. */
    int height = declared_height / 2;
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        return NULL;
    }

    if (palette_count == 0 && bpp <= 8) {
        palette_count = 1u << bpp;
    }

    const unsigned char *palette = data + header_size;
    size_t palette_bytes = (size_t)palette_count * 4;
    const unsigned char *pixels = palette + palette_bytes;

    /* Rows are padded to a multiple of four bytes. */
    size_t row_bits = (size_t)width * bpp;
    size_t row_stride = ((row_bits + 31) / 32) * 4;
    size_t mask_stride = (((size_t)width + 31) / 32) * 4;

    if (pixels + row_stride * (size_t)height > data + size) {
        return NULL;
    }
    const unsigned char *mask = pixels + row_stride * (size_t)height;
    bool has_mask = (mask + mask_stride * (size_t)height <= data + size);

    unsigned char *out = malloc((size_t)width * height * 4);
    if (out == NULL) {
        return NULL;
    }

    for (int y = 0; y < height; y++) {
        /* Rows run bottom to top. */
        const unsigned char *row = pixels + row_stride * (size_t)(height - 1 - y);
        const unsigned char *mask_row = has_mask
            ? mask + mask_stride * (size_t)(height - 1 - y) : NULL;

        for (int x = 0; x < width; x++) {
            unsigned char r = 0, g = 0, b = 0, a = 255;

            switch (bpp) {
            case 32:
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
                break;

            case 24:
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
                break;

            case 8: {
                unsigned char index = row[x];
                if ((size_t)index < palette_count) {
                    b = palette[index * 4 + 0];
                    g = palette[index * 4 + 1];
                    r = palette[index * 4 + 2];
                }
                break;
            }

            case 4: {
                /* Two pixels per byte, high nibble first. */
                unsigned char pair = row[x / 2];
                unsigned char index = (x % 2 == 0) ? (pair >> 4) : (pair & 0x0F);
                if ((size_t)index < palette_count) {
                    b = palette[index * 4 + 0];
                    g = palette[index * 4 + 1];
                    r = palette[index * 4 + 2];
                }
                break;
            }

            case 1: {
                unsigned char byte = row[x / 8];
                unsigned char index = (byte >> (7 - (x % 8))) & 1;
                if ((size_t)index < palette_count) {
                    b = palette[index * 4 + 0];
                    g = palette[index * 4 + 1];
                    r = palette[index * 4 + 2];
                }
                break;
            }

            default:
                free(out);
                return NULL;
            }

            /*
             * Below 32 bits there is no alpha channel, so transparency comes
             * from the mask that follows the image: a set bit means the pixel
             * is transparent.
             */
            if (bpp < 32 && mask_row != NULL) {
                unsigned char bit = (mask_row[x / 8] >> (7 - (x % 8))) & 1;
                a = bit ? 0 : 255;
            }

            unsigned char *px = out + ((size_t)y * width + x) * 4;
            px[0] = r;
            px[1] = g;
            px[2] = b;
            px[3] = a;
        }
    }

    *width_out = width;
    *height_out = height;
    return out;
}

/* --- ICO --- */

unsigned char *recon_ico_decode(const unsigned char *data, size_t size,
        int preferred_size, int *width_out, int *height_out) {
    if (data == NULL || size < 6) {
        return NULL;
    }

    if (read_u16(data) != 0 || read_u16(data + 2) != 1) {
        return NULL; /* not an icon */
    }

    int count = read_u16(data + 4);
    if (count <= 0 || size < 6 + (size_t)count * 16) {
        return NULL;
    }

    /*
     * Pick the entry closest to the size asked for, preferring a larger image
     * over a smaller one at equal distance: scaling down keeps detail, scaling
     * up invents it.
     */
    int best = -1;
    int best_score = 0;

    for (int i = 0; i < count; i++) {
        const unsigned char *entry = data + 6 + (size_t)i * 16;
        int w = entry[0] == 0 ? 256 : entry[0];
        int h = entry[1] == 0 ? 256 : entry[1];
        int dimension = w > h ? w : h;

        int score = dimension >= preferred_size
            ? 1000 - (dimension - preferred_size)
            : 500 - (preferred_size - dimension);

        if (best < 0 || score > best_score) {
            best = i;
            best_score = score;
        }
    }

    const unsigned char *entry = data + 6 + (size_t)best * 16;
    uint32_t bytes = read_u32(entry + 8);
    uint32_t offset = read_u32(entry + 12);

    if ((size_t)offset + bytes > size || bytes == 0) {
        return NULL;
    }

    const unsigned char *image = data + offset;

    /* Newer icons store PNG data directly. */
    static const unsigned char PNG_SIGNATURE[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (bytes > 8 && memcmp(image, PNG_SIGNATURE, 8) == 0) {
        int channels;
        return stbi_load_from_memory(image, (int)bytes,
            width_out, height_out, &channels, 4);
    }

    return decode_dib(image, bytes, width_out, height_out);
}
