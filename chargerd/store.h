// linht-chargerd: state file and cycle log in the state directory

#ifndef CHARGERD_STORE_H
#define CHARGERD_STORE_H

#include "telemetry.h"

#define STORE_STATE "state.json"
#define STORE_CYCLES "cycles.jsonl"
#define STORE_CYCLES_MAX 1000

// Atomic: temp file, fsync, rename. Returns 0 or a negative errno.
int store_save(const char *dir, const struct tel_saved *s);
// Returns 0, or a negative errno (-ENOENT when there is no file yet).
int store_load(const char *dir, struct tel_saved *s);
// Appends one JSON line, keeps the newest STORE_CYCLES_MAX lines.
int store_append_cycle(const char *dir, const struct tel_cycle *c);

#endif
