#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int vc_verbose = 0;

void vc_die(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("vc: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

void vc_warn(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("vc: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void vc_log(const char *fmt, ...) {
	if (!vc_verbose) return;
	va_list ap;
	va_start(ap, fmt);
	fputs("vc: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

uint32_t vc_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void vc_sleep_s(double seconds) {
	if (seconds <= 0) return;
	struct timespec ts = {
		.tv_sec = (time_t)seconds,
		.tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9),
	};
	nanosleep(&ts, NULL);
}

const char *vc_runtime_dir(void) {
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	return xdg && *xdg ? xdg : "/tmp";
}

const char *vc_box_dir(const char *name) {
	static char buf[512];
	snprintf(buf, sizeof buf, "%s/vctools/%s", vc_runtime_dir(), name);
	return buf;
}

int vc_mkdirs(const char *path) {
	char tmp[512];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/') continue;
		*p = 0;
		if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
		*p = '/';
	}
	return (mkdir(tmp, 0700) < 0 && errno != EEXIST) ? -1 : 0;
}

static const char *state_path(const char *name) {
	static char buf[600];
	snprintf(buf, sizeof buf, "%s/box", vc_box_dir(name));
	return buf;
}

const char *vc_state_get(const char *name, const char *key) {
	static char value[512];
	FILE *f = fopen(state_path(name), "r");
	if (!f) return NULL;

	char line[512];
	const char *found = NULL;
	size_t keylen = strlen(key);
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, key, keylen) || line[keylen] != '=') continue;
		snprintf(value, sizeof value, "%s", line + keylen + 1);
		char *nl = strchr(value, '\n');
		if (nl) *nl = 0;
		found = value;
		break;
	}
	fclose(f);
	return found;
}

int vc_state_get_int(const char *name, const char *key, int fallback) {
	const char *v = vc_state_get(name, key);
	return v ? atoi(v) : fallback;
}

// Rewrites the file with `key` replaced (or appended). The files are a handful
// of lines, so a full rewrite is simpler than in-place patching.
int vc_state_set(const char *name, const char *key, const char *value) {
	const char *path = state_path(name);
	char lines[32][512];
	int n = 0, replaced = 0;
	size_t keylen = strlen(key);

	FILE *f = fopen(path, "r");
	if (f) {
		while (n < 32 && fgets(lines[n], sizeof lines[0], f)) {
			if (!strncmp(lines[n], key, keylen) && lines[n][keylen] == '=') {
				snprintf(lines[n], sizeof lines[0], "%s=%s\n", key, value);
				replaced = 1;
			}
			n++;
		}
		fclose(f);
	}
	if (!replaced && n < 32) snprintf(lines[n++], sizeof lines[0], "%s=%s\n", key, value);

	f = fopen(path, "w");
	if (!f) return -1;
	for (int i = 0; i < n; i++) fputs(lines[i], f);
	fclose(f);
	return 0;
}

const char *vc_capture(const char *fmt, ...) {
	static char out[4096];
	char cmd[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof cmd, fmt, ap);
	va_end(ap);

	FILE *p = popen(cmd, "r");
	if (!p) return NULL;
	size_t len = fread(out, 1, sizeof out - 1, p);
	pclose(p);
	out[len] = 0;
	while (len && (out[len - 1] == '\n' || out[len - 1] == ' ')) out[--len] = 0;
	return out;
}

int vc_run(const char *fmt, ...) {
	char cmd[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof cmd, fmt, ap);
	va_end(ap);
	vc_log("run: %s", cmd);
	return system(cmd);
}
