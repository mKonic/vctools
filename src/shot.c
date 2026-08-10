#define _GNU_SOURCE
#include "shot.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "util.h"

struct capture_state {
	uint32_t width, height, format;
	int have_size, have_format, done, stopped;
	int ready, failed;
};

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t w,
                                uint32_t h) {
	struct capture_state *st = data;
	st->width = w;
	st->height = h;
	st->have_size = 1;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s,
                               uint32_t format) {
	struct capture_state *st = data;
	// Both ARGB8888 (0) and XRGB8888 (1) are BGRA byte order on little-endian.
	if (!st->have_format) {
		st->format = format;
		st->have_format = 1;
	}
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s,
                                  struct wl_array *dev) {}
static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s,
                                  uint32_t format, struct wl_array *mods) {}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *s) {
	((struct capture_state *)data)->done = 1;
}
static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s) {
	((struct capture_state *)data)->stopped = 1;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t tr) {}
static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y,
                         int32_t w, int32_t h) {}
static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *f,
                                    uint32_t hi, uint32_t lo, uint32_t nsec) {}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
	((struct capture_state *)data)->ready = 1;
}
static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
	((struct capture_state *)data)->failed = (int)reason ? (int)reason : -1;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

// Shared by the window and output paths; takes ownership of `source`.
static int capture_source(struct vc_wl *w, struct ext_image_capture_source_v1 *source,
                          struct vc_frame *out) {
	if (!w->capture_mgr || !w->shm) {
		vc_warn("compositor lacks ext-image-copy-capture or wl_shm");
		return -1;
	}

	struct capture_state st = { 0 };
	struct ext_image_copy_capture_session_v1 *session =
		ext_image_copy_capture_manager_v1_create_session(w->capture_mgr, source, 0);
	ext_image_copy_capture_session_v1_add_listener(session, &session_listener, &st);

	while (!st.done && !st.stopped)
		if (wl_display_dispatch(w->dpy) < 0) return -1;

	if (st.stopped || !st.have_size || !st.have_format) {
		vc_warn("capture session incomplete (size=%d format=%d)", st.have_size, st.have_format);
		return -1;
	}

	size_t stride = (size_t)st.width * 4;
	size_t size = stride * st.height;

	int fd = memfd_create("vc-shot", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		vc_warn("memfd for capture failed");
		return -1;
	}
	void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return -1;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(w->shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)st.width,
	                                                     (int32_t)st.height, (int32_t)stride,
	                                                     st.format);
	wl_shm_pool_destroy(pool);

	struct ext_image_copy_capture_frame_v1 *frame =
		ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, &st);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (int32_t)st.width,
	                                              (int32_t)st.height);
	ext_image_copy_capture_frame_v1_capture(frame);

	while (!st.ready && !st.failed)
		if (wl_display_dispatch(w->dpy) < 0) break;

	int rc = 0;
	if (st.failed) {
		vc_warn("capture failed, reason=%d", st.failed);
		rc = -1;
	} else {
		out->data = malloc(size);
		if (!out->data) {
			rc = -1;
		} else {
			memcpy(out->data, pixels, size);
			out->w = (int)st.width;
			out->h = (int)st.height;
			out->stride = (int)stride;
			out->format = st.format;
		}
	}

	munmap(pixels, size);
	close(fd);
	wl_buffer_destroy(buffer);
	ext_image_copy_capture_frame_v1_destroy(frame);
	ext_image_copy_capture_session_v1_destroy(session);
	ext_image_capture_source_v1_destroy(source);
	return rc;
}

int vc_shot_window(struct vc_wl *w, struct vc_toplevel *t, struct vc_frame *out) {
	if (!w->window_source_mgr) {
		vc_warn("compositor has no foreign-toplevel capture source");
		return -1;
	}
	memset(out, 0, sizeof *out);
	struct ext_image_capture_source_v1 *src =
		ext_foreign_toplevel_image_capture_source_manager_v1_create_source(w->window_source_mgr,
		                                                                   t->handle);
	return capture_source(w, src, out);
}

int vc_shot_output(struct vc_wl *w, struct vc_output *o, struct vc_frame *out) {
	if (!w->output_source_mgr) {
		vc_warn("compositor has no output capture source");
		return -1;
	}
	memset(out, 0, sizeof *out);
	struct ext_image_capture_source_v1 *src =
		ext_output_image_capture_source_manager_v1_create_source(w->output_source_mgr, o->output);
	return capture_source(w, src, out);
}

void vc_frame_free(struct vc_frame *f) {
	free(f->data);
	f->data = NULL;
}

double vc_frame_diff(const struct vc_frame *a, const struct vc_frame *b) {
	if (!a->data || !b->data || a->w != b->w || a->h != b->h) return 255.0;

	uint64_t sum = 0;
	for (int y = 0; y < a->h; y++) {
		const uint8_t *ra = a->data + (size_t)y * a->stride;
		const uint8_t *rb = b->data + (size_t)y * b->stride;
		for (int x = 0; x < a->w * 4; x += 4) {
			for (int c = 0; c < 3; c++) { // ignore alpha: it is undefined for XRGB
				int d = (int)ra[x + c] - (int)rb[x + c];
				sum += (uint64_t)(d < 0 ? -d : d);
			}
		}
	}
	return (double)sum / ((double)a->w * a->h * 3.0);
}
