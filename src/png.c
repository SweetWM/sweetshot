#include "png.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// PNG caps chunk lengths and dimensions
#define PNG_MAX 0x7fffffffU

enum {
	FILTER_UP = 2,
	COLOR_RGB = 2,
	COLOR_RGBA = 6,
	CHUNK_OVERHEAD = 12, // length, type, CRC
	IHDR_LEN = 13,
};

static const uint8_t signature[8] = {
	0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

static void put32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint8_t *finish_chunk(uint8_t *p, const char type[4], uint32_t len) {
	put32(p, len);
	memcpy(p + 4, type, 4);
	uLong crc = crc32(0, p + 4, 4 + len);
	put32(p + 8 + len, (uint32_t)crc);
	return p + CHUNK_OVERHEAD + len;
}

static void swizzle_xrgb(uint8_t *dst, const uint8_t *src, uint32_t width) {
	for (uint32_t x = 0; x < width; x++, src += 4, dst += 3) {
		dst[0] = src[2];
		dst[1] = src[1];
		dst[2] = src[0];
	}
}

// wl_shm alpha is premultiplied, PNG alpha is straight
static void swizzle_argb(uint8_t *dst, const uint8_t *src, uint32_t width) {
	for (uint32_t x = 0; x < width; x++, src += 4, dst += 4) {
		unsigned a = src[3];
		if (a == 255) {
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
		} else if (a == 0) {
			dst[0] = dst[1] = dst[2] = 0;
		} else {
			for (int i = 0; i < 3; i++) {
				unsigned c = (src[2 - i] * 255U + a / 2) / a;
				dst[i] = (uint8_t)(c > 255 ? 255 : c);
			}
		}
		dst[3] = (uint8_t)a;
	}
}

bool png_encode(const struct png_image *img, uint8_t **out, size_t *len) {
	bool rgba = img->format == PNG_ARGB8888;
	size_t bpp = rgba ? 4 : 3;
	if (img->width == 0 || img->height == 0 || img->width > PNG_MAX ||
		img->height > PNG_MAX || img->stride / 4 < img->width) {
		log_error("cannot encode a %ux%u image with stride %zu",
			img->width, img->height, img->stride);
		return false;
	}
	size_t row_len = img->width * bpp;
	size_t filtered_len;
	if (__builtin_mul_overflow(row_len + 1, img->height, &filtered_len)) {
		log_error("image is too large to encode");
		return false;
	}

	bool ok = false;
	uint8_t *buf = NULL;
	uint8_t *rows = NULL;
	z_stream zs = {0};
	if (deflateInit(&zs, 1) != Z_OK) {
		log_error("out of memory");
		return false;
	}
	// one IDAT, so its whole compressed stream must fit one chunk length
	uLong bound = deflateBound(&zs, filtered_len);
	if (bound > PNG_MAX) {
		log_error("image is too large for one PNG data chunk");
		goto out;
	}
	size_t header_len = sizeof(signature) + CHUNK_OVERHEAD + IHDR_LEN;
	size_t cap = header_len + CHUNK_OVERHEAD + bound + CHUNK_OVERHEAD;
	buf = malloc(cap);
	// previous, current and filtered row, the previous row of the first
	// row is all zero, so Up leaves it unchanged
	rows = calloc(3, row_len + 1);
	if (!buf || !rows) {
		log_error("out of memory");
		goto out;
	}
	uint8_t *prev = rows;
	uint8_t *cur = rows + row_len + 1;
	uint8_t *filt = rows + 2 * (row_len + 1);

	memcpy(buf, signature, sizeof(signature));
	uint8_t *ihdr = buf + sizeof(signature);
	put32(ihdr + 8, img->width);
	put32(ihdr + 12, img->height);
	ihdr[16] = 8; // bit depth
	ihdr[17] = rgba ? COLOR_RGBA : COLOR_RGB;
	ihdr[18] = 0; // deflate
	ihdr[19] = 0; // adaptive filtering, here always Up
	ihdr[20] = 0; // no interlace
	uint8_t *idat = finish_chunk(ihdr, "IHDR", IHDR_LEN);

	// rows stream through deflate, no full-size filtered copy exists
	zs.next_out = idat + 8;
	zs.avail_out = (uInt)bound;
	filt[0] = FILTER_UP;
	for (uint32_t y = 0; y < img->height; y++) {
		const uint8_t *src = img->data + (size_t)y * img->stride;
		if (rgba) {
			swizzle_argb(cur, src, img->width);
		} else {
			swizzle_xrgb(cur, src, img->width);
		}
		for (size_t i = 0; i < row_len; i++) {
			filt[i + 1] = (uint8_t)(cur[i] - prev[i]);
		}
		zs.next_in = filt;
		zs.avail_in = (uInt)(row_len + 1);
		if (deflate(&zs, Z_NO_FLUSH) != Z_OK || zs.avail_in != 0) {
			log_error("png compression failed");
			goto out;
		}
		uint8_t *tmp = prev;
		prev = cur;
		cur = tmp;
	}
	if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
		log_error("png compression failed");
		goto out;
	}

	uint8_t *iend = finish_chunk(idat, "IDAT", (uint32_t)zs.total_out);
	uint8_t *end = finish_chunk(iend, "IEND", 0);
	*len = (size_t)(end - buf);
	// gives back the unused deflateBound tail, keeping it is harmless
	uint8_t *shrunk = realloc(buf, *len);
	*out = shrunk ? shrunk : buf;
	buf = NULL;
	ok = true;

out:
	deflateEnd(&zs);
	free(rows);
	free(buf);
	return ok;
}

struct png_image png_upright(
	const struct png_image *img, uint32_t transform, uint8_t *dst) {
	size_t w = img->width;
	size_t h = img->height;
	// odd values are 90, 270, flipped-90 and flipped-270
	bool swap = transform & 1;
	size_t ow = swap ? h : w;
	for (size_t y = 0; y < h; y++) {
		const uint8_t *row = img->data + y * img->stride;
		for (size_t x = 0; x < w; x++) {
			size_t ox;
			size_t oy;
			switch (transform) {
			case 1: // 90
				ox = h - 1 - y;
				oy = x;
				break;
			case 2: // 180
				ox = w - 1 - x;
				oy = h - 1 - y;
				break;
			case 3: // 270
				ox = y;
				oy = w - 1 - x;
				break;
			case 4: // flipped
				ox = w - 1 - x;
				oy = y;
				break;
			case 5: // flipped-90
				ox = y;
				oy = x;
				break;
			case 6: // flipped-180
				ox = x;
				oy = h - 1 - y;
				break;
			case 7: // flipped-270
				ox = h - 1 - y;
				oy = w - 1 - x;
				break;
			default:
				ox = x;
				oy = y;
				break;
			}
			memcpy(dst + (oy * ow + ox) * 4, row + x * 4, 4);
		}
	}
	return (struct png_image){
		.width = (uint32_t)ow,
		.height = (uint32_t)(swap ? w : h),
		.stride = ow * 4,
		.format = img->format,
		.data = dst,
	};
}
