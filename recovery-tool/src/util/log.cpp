/*
 * log.cpp - Multi-level logging
 *
 * Levels (controlled by -v flags):
 *   0 (default): progress every 100th seed + stage summaries + errors
 *   1 (-v):      important events (file recovered, seed skipped with reason)
 *   2 (-vv):     moderate detail (header parse, chain building, candidate counts)
 *   3 (-vvv):    everything (per-candidate scores, Huffman results, every decision)
 *
 * Debug log file: always gets everything regardless of verbosity.
 */
#include "sdrecov.h"
#include <cstdarg>
#include <ctime>

static const char *timestamp()
{
    static char buf[32];
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    strftime(buf, sizeof(buf), "%H:%M:%S", t);
    return buf;
}

void log_init(RecoveryContext &ctx)
{
    if (!ctx.output_dir.empty()) {
        auto path = ctx.output_dir + "/debug.log";
        ctx.debug_log = fopen(path.c_str(), "w");
        if (!ctx.debug_log)
            fprintf(stderr, "warning: cannot open %s\n", path.c_str());
    }
}

/* Always write to debug log file; print to stderr if verbosity >= min_level */
static void log_at_level(const RecoveryContext &ctx, int min_level,
                          const char *prefix, const char *fmt, va_list ap)
{
    /* Always to debug log */
    if (ctx.debug_log) {
        fprintf(ctx.debug_log, "%s %s", timestamp(), prefix);
        va_list ap2;
        va_copy(ap2, ap);
        vfprintf(ctx.debug_log, fmt, ap2);
        va_end(ap2);
        fprintf(ctx.debug_log, "\n");
        fflush(ctx.debug_log);
    }

    /* To stderr if verbosity is high enough */
    if (ctx.verbosity >= min_level) {
        fprintf(stderr, "%s", prefix);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
}

/* Level 0: always shown (progress, stage boundaries, final results) */
void log_progress(const RecoveryContext &ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_at_level(ctx, 0, "", fmt, ap);
    va_end(ap);
}

/* Level 1 (-v): important events */
void log_info(const RecoveryContext &ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_at_level(ctx, 1, "  ", fmt, ap);
    va_end(ap);
}

/* Level 2 (-vv): moderate detail */
void log_detail(const RecoveryContext &ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_at_level(ctx, 2, "    ", fmt, ap);
    va_end(ap);
}

/* Level 3 (-vvv): everything */
void log_debug(const RecoveryContext &ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_at_level(ctx, 3, "      ", fmt, ap);
    va_end(ap);
}

/* Errors: always shown */
void log_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}
