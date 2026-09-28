/*
 * WhaleJS diagnostic switches - build-time, not runtime configuration.
 *
 * Every WHALE_* environment variable read here is a development diagnostic
 * (dump the generated source, report why a function did not compile, count
 * struct helper calls). They are debugging aids for working ON this engine,
 * not configuration for the programs that run on it, so they only exist in a
 * debug build: configure with
 *
 *     xmake f -p mingw -m release --whale_debug=y
 *
 * and WHALE_DEBUG is defined; without it every switch reads as "off", the
 * prints are absent from the binary, and no environment lookup happens.
 * See notes <61> for the full inventory and the reason each exists.
 */
#ifndef WHALE_DBG_H
#define WHALE_DBG_H

#ifdef WHALE_DEBUG

#include <stdlib.h>
#define whale_dbg_env(name) (getenv(name) != NULL)

#else

#define whale_dbg_env(name) 0

#endif

#endif /* WHALE_DBG_H */
