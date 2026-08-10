// Minimal PNG writer, so captures land as .png without shelling out to magick.
#define _GNU_SOURCE
#include "png.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
	uint8_t hdr[8];
	be32(hdr, (uint32_t)len);
	memcpy(hdr + 4, type, 4);
	if (fwrite(hdr, 1, 8, f) != 8) return -1;
	if (len && fwrite(data, 1, len, f) != len) return -1;

	uLong crc = crc32(0, (const Bytef *)type, 4);
	if (len) crc = crc32(crc, data, (uInt)len);
	uint8_t crcbuf[4];
	be32(crcbuf, (uint32_t)crc);
	return fwrite(crcbuf, 1, 4, f) == 4 ? 0 : -1;
}

int vc_png_write(const char *path, const uint8_t *bgra, int w, int h, int stride) {
	// PNG wants each scanline prefixed with a filter byte; filter 0 (none) keeps
	// this simple and still compresses well for UI screenshots.
	size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
	uint8_t *raw = malloc(raw_len);
	if (!raw) return -1;

	size_t o = 0;
	for (int y = 0; y < h; y++) {
		raw[o++] = 0;
		const uint8_t *row = bgra + (size_t)y * stride;
		for (int x = 0; x < w; x++) {
			raw[o++] = row[x * 4 + 2]; // R
			raw[o++] = row[x * 4 + 1]; // G
			raw[o++] = row[x * 4 + 0]; // B
		}
	}

	uLongf comp_len = compressBound((uLong)raw_len);
	uint8_t *comp = malloc(comp_len);
	if (!comp || compress2(comp, &comp_len, raw, (uLong)raw_len, 6) != Z_OK) {
		free(raw);
		free(comp);
		return -1;
	}
	free(raw);

	FILE *f = fopen(path, "wb");
	if (!f) {
		free(comp);
		return -1;
	}

	static const uint8_t sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
	int rc = fwrite(sig, 1, 8, f) == 8 ? 0 : -1;

	uint8_t ihdr[13];
	be32(ihdr, (uint32_t)w);
	be32(ihdr + 4, (uint32_t)h);
	ihdr[8] = 8;  // bit depth
	ihdr[9] = 2;  // colour type: truecolour
	ihdr[10] = 0; // deflate
	ihdr[11] = 0; // adaptive filtering
	ihdr[12] = 0; // no interlace

	if (!rc) rc = write_chunk(f, "IHDR", ihdr, sizeof ihdr);
	if (!rc) rc = write_chunk(f, "IDAT", comp, comp_len);
	if (!rc) rc = write_chunk(f, "IEND", NULL, 0);

	fclose(f);
	free(comp);
	return rc;
}
