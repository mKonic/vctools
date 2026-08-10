// Small shared helpers: logging, paths, the box registry file format.
#ifndef VC_UTIL_H
#define VC_UTIL_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

extern int vc_verbose;

void        vc_die(const char *fmt, ...);
void        vc_warn(const char *fmt, ...);
void        vc_log(const char *fmt, ...); // only when -v
uint32_t    vc_now_ms(void);
void        vc_sleep_s(double seconds);

// Directory holding per-box state: $XDG_RUNTIME_DIR/vctools/<name>
const char *vc_box_dir(const char *name);
const char *vc_runtime_dir(void);
int         vc_mkdirs(const char *path);

// The box state file is a flat key=value list; these read and write it.
int         vc_state_set(const char *name, const char *key, const char *value);
// Returns a pointer to a static buffer, or NULL when the key is absent.
const char *vc_state_get(const char *name, const char *key);
int         vc_state_get_int(const char *name, const char *key, int fallback);

// Run a command, returning its trimmed stdout in a static buffer (NULL on failure).
const char *vc_capture(const char *fmt, ...);
int         vc_run(const char *fmt, ...); // returns exit status

#endif
