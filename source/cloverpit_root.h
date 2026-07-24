/* cloverpit_root.h -- runtime data-root resolution.
 *
 * The SD folder name is NOT compiled in. cp_resolve_game_root() must be called
 * first thing in main(), before any file access and before the first log write,
 * since the log path is derived from the resolved root.
 */
#ifndef CLOVERPIT_ROOT_H
#define CLOVERPIT_ROOT_H

/* Resolve and return the data root. Idempotent; safe to call more than once. */
const char *cp_resolve_game_root(int argc, char **argv);

/* Resolved data root, e.g. "sdmc:/switch/cloverpit". Falls back to the
 * compiled-in GAME_HOME if nothing validated. Never NULL. */
const char *cp_game_root(void);

/* "<root>/debug.log". Never NULL. */
const char *cp_log_path(void);

/* Log how the root was chosen. Call after resolution. */
void cp_root_report(int argc, char **argv);

#endif /* CLOVERPIT_ROOT_H */
