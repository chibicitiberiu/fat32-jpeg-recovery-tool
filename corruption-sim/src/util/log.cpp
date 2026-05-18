/*
 * log.cpp - Multi-level logging
 *
 * Levels (controlled by -v flags):
 *   0 (default): stage progress + final summary + errors
 *   1 (-v):      file operations, corruption pass stats, profile applied
 *   2 (-vv):     individual folder creation, file placements, block selections
 *   3 (-vvv):    every bit flip, every byte modified, full subprocess output
 *
 * Debug log file: always gets everything regardless of verbosity.
 */
#include "corrsim.h"
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

void log_init(SimContext &ctx)
{
    // Debug log goes next to the output image
    auto dot = ctx.cfg.image_path.rfind('.');
    std::string base = (dot != std::string::npos)
        ? ctx.cfg.image_path.substr(0, dot)
        : ctx.cfg.image_path;
    auto path = base + ".debug.log";
    ctx.debug_log = fopen(path.c_str(), "w");
    if (!ctx.debug_log)
        fprintf(stderr, "warning: cannot open %s\n", path.c_str());
}

void log_close(SimContext &ctx)
{
    if (ctx.debug_log) {
        fclose(ctx.debug_log);
        ctx.debug_log = nullptr;
    }
}

static void log_at_level(const SimContext &ctx, int min_level,
                          const char *prefix, const char *fmt, va_list ap)
{
    if (ctx.debug_log) {
        fprintf(ctx.debug_log, "%s %s", timestamp(), prefix);
        va_list ap2;
        va_copy(ap2, ap);
        vfprintf(ctx.debug_log, fmt, ap2);
        va_end(ap2);
        fprintf(ctx.debug_log, "\n");
        fflush(ctx.debug_log);
    }
    if (ctx.cfg.verbosity >= min_level) {
        fprintf(stderr, "%s", prefix);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
}

void log_progress(const SimContext &ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    log_at_level(ctx, 0, "", fmt, ap);
    va_end(ap);
}

void log_info(const SimContext &ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    log_at_level(ctx, 1, "  ", fmt, ap);
    va_end(ap);
}

void log_detail(const SimContext &ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    log_at_level(ctx, 2, "    ", fmt, ap);
    va_end(ap);
}

void log_debug(const SimContext &ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    log_at_level(ctx, 3, "      ", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}
