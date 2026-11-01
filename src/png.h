#ifndef SWEETSHOT_PNG_H
#define SWEETSHOT_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum png_format {
	PNG_XRGB8888,
	PNG_ARGB8888,
};

struct png_image {
	uint32_t width;
	uint32_t height;
	size_t stride;
	enum png_format format;
	const uint8_t *data;
};

bool png_encode(const struct png_image *img, uint8_t **out, size_t *len);

struct png_image png_upright(
	const struct png_image *img, uint32_t transform, uint8_t *dst);

#endif
