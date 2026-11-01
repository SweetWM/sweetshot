#ifndef SWEETSHOT_SAVE_H
#define SWEETSHOT_SAVE_H

#include <stddef.h>
#include <stdint.h>

char *save_default_dir(void);

char *save_png(const char *dir, const uint8_t *data, size_t len);

#endif
