#ifndef Y1_LOG_H
#define Y1_LOG_H

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "settings.h"

static inline void y1_log(const char *msg)
{
    if (!global_settings.y1_logging_enabled)
        return;

    int fd = open("/mnt/sdcard/y1_debug.txt",
                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd >= 0) {
        (void)!write(fd, msg, strlen(msg));
        close(fd);
    }
}

#endif
