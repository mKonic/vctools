#ifndef VC_PNG_H
#define VC_PNG_H

#include <stdint.h>
#include <stdio.h>

// Writes a BGRA (little-endian ARGB8888/XRGB8888) buffer as an RGB PNG.
int vc_png_write(const char *path, const uint8_t *bgra, int w, int h, int stride);

#endif
