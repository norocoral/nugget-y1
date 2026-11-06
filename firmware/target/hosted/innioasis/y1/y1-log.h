#ifndef Y1_LOG_H
#define Y1_LOG_H

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

#include "settings.h"

static inline void y1_log_raw(const char *msg, size_t len)
{
    static int kfd = -1;
    static int dfd = -1;

    if (!global_settings.y1_logging_enabled) {
        if (dfd >= 0) {
            close(dfd);
            dfd = -1;
        }
        return;
    }

    if (len == 0)
        return;

    (void)!write(1, msg, len);

    if (kfd < 0)
        kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0)
        (void)!write(kfd, msg, len);

    if (dfd < 0) {
        dfd = open("/mnt/sdcard/y1_debug.txt",
                   O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    }
    if (dfd >= 0) {
        if (write(dfd, msg, len) < 0) {
            close(dfd);
            dfd = -1;
        }
    }
}

static inline void y1_log(const char *msg)
{
    if (!global_settings.y1_logging_enabled || !msg)
        return;
    y1_log_raw(msg, strlen(msg));
}

static inline void y1_logf(const char *fmt, ...)
{
    if (!global_settings.y1_logging_enabled)
        return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len > 0) {
        if ((size_t)len >= sizeof(buf))
            len = sizeof(buf) - 1;
        y1_log_raw(buf, (size_t)len);
    }
}

#endif
