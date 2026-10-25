#ifndef Y1_LOG_H
#define Y1_LOG_H

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

#include "settings.h"

static inline void y1_log(const char *msg)
{
    if (!global_settings.y1_logging_enabled)
        return;

    size_t len = strlen(msg);
    if (len > 0) {
        (void)!write(1, msg, len);

        int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (kfd >= 0) {
            (void)!write(kfd, msg, len);
            close(kfd);
        }

        int fd = open("/mnt/sdcard/y1_debug.txt",
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if (fd >= 0) {
            (void)!write(fd, msg, len);
            close(fd);
        }
    }
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
        (void)!write(1, buf, len);

        int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (kfd >= 0) {
            (void)!write(kfd, buf, len);
            close(kfd);
        }

        int fd = open("/mnt/sdcard/y1_debug.txt",
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if (fd >= 0) {
            (void)!write(fd, buf, len);
            close(fd);
        }
    }
}

#endif
