#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#define EMU_W 480
#define EMU_H 360
#define EMU_BPP 32
#define EMU_STRIDE (EMU_W * 4)
#define EMU_FB_SZ (EMU_STRIDE * EMU_H)

static int (*real_open)(const char *, int, ...) = NULL;
static ssize_t (*real_write)(int, const void *, size_t) = NULL;
static int (*real_close)(int) = NULL;
static FILE *(*real_fopen)(const char *, const char *) = NULL;
static DIR *(*real_opendir)(const char *) = NULL;
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static int (*real_access)(const char *, int) = NULL;
static int (*real_stat)(const char *, struct stat *) = NULL;
static int (*real_lstat)(const char *, struct stat *) = NULL;
static int (*real_mkdir)(const char *, mode_t) = NULL;
static int (*real_unlink)(const char *) = NULL;
static int (*real_rename)(const char *, const char *) = NULL;
static int (*real_uname)(struct utsname *) = NULL;
static long (*real_sysconf)(int) = NULL;

static int g_fb_fd = -1;
static int g_ev0_fd = -1;
static int g_eac_fd = -1;
static int g_eac_fifo_fd = -1;
static int g_eac_sampr = 44100;

static void init_syms(void)
{
    if (!real_open)    real_open    = dlsym(RTLD_NEXT, "open");
    if (!real_write)   real_write   = dlsym(RTLD_NEXT, "write");
    if (!real_close)   real_close   = dlsym(RTLD_NEXT, "close");
    if (!real_fopen)   real_fopen   = dlsym(RTLD_NEXT, "fopen");
    if (!real_opendir) real_opendir = dlsym(RTLD_NEXT, "opendir");
    if (!real_ioctl)   real_ioctl   = dlsym(RTLD_NEXT, "ioctl");
    if (!real_access)  real_access  = dlsym(RTLD_NEXT, "access");
    if (!real_stat)    real_stat    = dlsym(RTLD_NEXT, "stat");
    if (!real_lstat)   real_lstat   = dlsym(RTLD_NEXT, "lstat");
    if (!real_mkdir)   real_mkdir   = dlsym(RTLD_NEXT, "mkdir");
    if (!real_unlink)  real_unlink  = dlsym(RTLD_NEXT, "unlink");
    if (!real_rename)  real_rename  = dlsym(RTLD_NEXT, "rename");
    if (!real_uname)   real_uname   = dlsym(RTLD_NEXT, "uname");
    if (!real_sysconf) real_sysconf = dlsym(RTLD_NEXT, "sysconf");
}

static const char *emu_dir(void)
{
    const char *d = getenv("Y1_EMU_DIR");
    return (d && d[0]) ? d : "/tmp/y1_emu";
}

static const char *map_path(const char *path, char *buf, size_t len,
                            bool *is_fb, bool *is_ev0, bool *is_eac)
{
    *is_fb = false;
    *is_ev0 = false;
    *is_eac = false;
    if (!path) return path;

    if (strcmp(path, "/dev/graphics/fb0") == 0 || strcmp(path, "/dev/fb0") == 0) {
        *is_fb = true;
        snprintf(buf, len, "%s/fb0", emu_dir());
        return buf;
    }
    if (strcmp(path, "/dev/input/event0") == 0) {
        *is_ev0 = true;
        snprintf(buf, len, "%s/input_fifo", emu_dir());
        return buf;
    }
    if (strcmp(path, "/dev/eac") == 0) {
        *is_eac = true;
        snprintf(buf, len, "/dev/null");
        return buf;
    }
    if (strncmp(path, "/dev/input/event", 16) == 0 ||
        strcmp(path, "/dev/fm") == 0 ||
        strcmp(path, "/dev/rtc0") == 0) {
        snprintf(buf, len, "/dev/null");
        return buf;
    }
    if (strcmp(path, "/sys/class/power_supply/battery/capacity") == 0) {
        snprintf(buf, len, "%s/bat_cap", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/class/power_supply/battery/status") == 0) {
        snprintf(buf, len, "%s/bat_status", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/class/power_supply/battery/voltage_now") == 0) {
        snprintf(buf, len, "%s/bat_volt", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/class/power_supply/battery/current_now") == 0) {
        snprintf(buf, len, "%s/bat_curr", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/class/power_supply/usb/online") == 0) {
        snprintf(buf, len, "%s/usb_online", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/class/switch/h2w/state") == 0) {
        snprintf(buf, len, "%s/h2w_state", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/class/leds/lcd-backlight/brightness") == 0) {
        snprintf(buf, len, "%s/bl_brightness", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/devices/virtual/timed_output/vibrator/enable") == 0 ||
        strcmp(path, "/sys/class/timed_output/vibrator/enable") == 0) {
        snprintf(buf, len, "%s/vibrate", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") == 0 ||
        strcmp(path, "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq") == 0 ||
        strcmp(path, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq") == 0 ||
        strcmp(path, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed") == 0 ||
        strcmp(path, "/proc/cpufreq/cpufreq_cur_freq") == 0) {
        snprintf(buf, len, "%s/cpu_freq", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") == 0) {
        snprintf(buf, len, "%s/cpu_gov", emu_dir());
        return buf;
    }
    if (strcmp(path, "/sys/devices/system/cpu/cpu1/online") == 0) {
        snprintf(buf, len, "%s/cpu1_online", emu_dir());
        return buf;
    }
    if (strncmp(path, "/sys/devices/system/cpu/", 24) == 0 ||
        strncmp(path, "/proc/cpufreq/", 14) == 0) {
        snprintf(buf, len, "/dev/null");
        return buf;
    }
    if (strcmp(path, "/proc/meminfo") == 0) {
        snprintf(buf, len, "%s/meminfo", emu_dir());
        return buf;
    }
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        snprintf(buf, len, "%s/cpuinfo", emu_dir());
        return buf;
    }
    if (strcmp(path, "/proc/lcm") == 0) {
        snprintf(buf, len, "%s/proc_lcm", emu_dir());
        return buf;
    }

    const char *sd = getenv("Y1_EMU_SDCARD");
    if (sd && sd[0]) {
        if (strncmp(path, "/mnt/sdcard", 11) == 0 &&
            (path[11] == '\0' || path[11] == '/')) {
            snprintf(buf, len, "%s%s", sd, path + 11);
            return buf;
        }
        static const char *sd_links[] = {
            "/.rockbox", "/.system", "/.meui", "/Music", "/Photos",
            "/Playlists", "/Videos", "/Screenshots", "/Recordings", NULL
        };
        for (int i = 0; sd_links[i]; i++) {
            size_t sl = strlen(sd_links[i]);
            if (strncmp(path, sd_links[i], sl) == 0 &&
                (path[sl] == '\0' || path[sl] == '/')) {
                snprintf(buf, len, "%s%s", sd, path);
                return buf;
            }
        }
    }
    return path;
}

int open(const char *pathname, int flags, ...)
{
    init_syms();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    char mapped[512];
    bool is_fb = false, is_ev0 = false, is_eac = false;
    const char *p = map_path(pathname, mapped, sizeof(mapped), &is_fb, &is_ev0, &is_eac);
    int fd = (flags & O_CREAT) ? real_open(p, flags, mode) : real_open(p, flags);
    if (fd >= 0 && is_fb) {
        g_fb_fd = fd;
        (void)!ftruncate(fd, EMU_FB_SZ);
    } else if (fd >= 0 && is_ev0) {
        g_ev0_fd = fd;
    } else if (fd >= 0 && is_eac) {
        g_eac_fd = fd;
    }
    return fd;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    init_syms();
    if (fd >= 0 && fd == g_eac_fd) {
        if (g_eac_fifo_fd < 0) {
            char apath[256];
            snprintf(apath, sizeof(apath), "%s/audio_fifo", emu_dir());
            g_eac_fifo_fd = real_open(apath, O_WRONLY | O_NONBLOCK);
        }
        if (g_eac_fifo_fd >= 0 && count > 0) {
            ssize_t w = real_write(g_eac_fifo_fd, buf, count);
            if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                real_close(g_eac_fifo_fd);
                g_eac_fifo_fd = -1;
            }
        }
        /* Pace PCM writes at real-time audio rate (S16LE stereo) */
        if (count > 0 && g_eac_sampr > 0) {
            useconds_t us = (useconds_t)(((uint64_t)count * 1000000ULL) / ((uint64_t)g_eac_sampr * 4ULL));
            if (us > 0 && us < 500000U) usleep(us);
        }
        return (ssize_t)count;
    }
    return real_write(fd, buf, count);
}

int close(int fd)
{
    init_syms();
    if (fd >= 0 && fd == g_eac_fd) {
        g_eac_fd = -1;
        if (g_eac_fifo_fd >= 0) {
            real_close(g_eac_fifo_fd);
            g_eac_fifo_fd = -1;
        }
    }
    if (fd >= 0 && fd == g_fb_fd) g_fb_fd = -1;
    if (fd >= 0 && fd == g_ev0_fd) g_ev0_fd = -1;
    return real_close(fd);
}

int access(const char *pathname, int mode)
{
    init_syms();
    char mapped[512];
    bool is_fb = false, is_ev0 = false, is_eac = false;
    const char *p = map_path(pathname, mapped, sizeof(mapped), &is_fb, &is_ev0, &is_eac);
    return real_access(p, mode);
}

int stat(const char *pathname, struct stat *statbuf)
{
    init_syms();
    char mapped[512];
    bool is_fb = false, is_ev0 = false, is_eac = false;
    const char *p = map_path(pathname, mapped, sizeof(mapped), &is_fb, &is_ev0, &is_eac);
    return real_stat(p, statbuf);
}

int lstat(const char *pathname, struct stat *statbuf)
{
    init_syms();
    char mapped[512];
    bool is_fb = false, is_ev0 = false, is_eac = false;
    const char *p = map_path(pathname, mapped, sizeof(mapped), &is_fb, &is_ev0, &is_eac);
    return real_lstat(p, statbuf);
}

FILE *fopen(const char *pathname, const char *mode)
{
    init_syms();
    char mapped[512];
    bool is_fb = false, is_ev0 = false, is_eac = false;
    const char *p = map_path(pathname, mapped, sizeof(mapped), &is_fb, &is_ev0, &is_eac);
    return real_fopen(p, mode);
}

DIR *opendir(const char *name)
{
    init_syms();
    char mapped[512];
    bool is_fb = false, is_ev0 = false, is_eac = false;
    const char *p = map_path(name, mapped, sizeof(mapped), &is_fb, &is_ev0, &is_eac);
    return real_opendir(p);
}

int mkdir(const char *pathname, mode_t mode)
{
    init_syms();
    char mapped[512];
    bool is_fb = false, is_ev0 = false, is_eac = false;
    const char *p = map_path(pathname, mapped, sizeof(mapped), &is_fb, &is_ev0, &is_eac);
    return real_mkdir(p, mode);
}

int unlink(const char *pathname)
{
    init_syms();
    char mapped[512];
    bool is_fb = false, is_ev0 = false, is_eac = false;
    const char *p = map_path(pathname, mapped, sizeof(mapped), &is_fb, &is_ev0, &is_eac);
    return real_unlink(p);
}

int rename(const char *oldpath, const char *newpath)
{
    init_syms();
    char m1[512], m2[512];
    bool f1 = false, e1 = false, a1 = false, f2 = false, e2 = false, a2 = false;
    const char *p1 = map_path(oldpath, m1, sizeof(m1), &f1, &e1, &a1);
    const char *p2 = map_path(newpath, m2, sizeof(m2), &f2, &e2, &a2);
    return real_rename(p1, p2);
}

int uname(struct utsname *buf)
{
    init_syms();
    int rc = real_uname ? real_uname(buf) : 0;
    if (rc == 0 && buf) {
        snprintf(buf->sysname, sizeof(buf->sysname), "Linux");
        snprintf(buf->nodename, sizeof(buf->nodename), "y1");
        snprintf(buf->release, sizeof(buf->release), "3.4.5");
        snprintf(buf->version, sizeof(buf->version), "#7 SMP PREEMPT Fri Aug 1 11:22:53 CST 2025");
        snprintf(buf->machine, sizeof(buf->machine), "armv7l");
    }
    return rc;
}

long sysconf(int name)
{
    init_syms();
    if (name == _SC_NPROCESSORS_CONF)
        return 2;
    if (name == _SC_NPROCESSORS_ONLN) {
        char p[256];
        snprintf(p, sizeof(p), "%s/cpu1_online", emu_dir());
        FILE *f = real_fopen(p, "r");
        int on = 1;
        if (f) { (void)!fscanf(f, "%d", &on); fclose(f); }
        return on ? 2 : 1;
    }
    return real_sysconf ? real_sysconf(name) : -1;
}

int ioctl(int fd, unsigned long request, ...)
{
    init_syms();
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (fd == g_fb_fd) {
        if (request == FBIOGET_VSCREENINFO && arg) {
            struct fb_var_screeninfo *v = (struct fb_var_screeninfo *)arg;
            memset(v, 0, sizeof(*v));
            v->xres = EMU_W;
            v->yres = EMU_H;
            v->xres_virtual = EMU_W;
            v->yres_virtual = EMU_H;
            v->bits_per_pixel = EMU_BPP;
            v->red.offset = 16;   v->red.length = 8;
            v->green.offset = 8;  v->green.length = 8;
            v->blue.offset = 0;   v->blue.length = 8;
            v->transp.offset = 24;v->transp.length = 8;
            return 0;
        }
        if (request == FBIOGET_FSCREENINFO && arg) {
            struct fb_fix_screeninfo *f = (struct fb_fix_screeninfo *)arg;
            memset(f, 0, sizeof(*f));
            snprintf(f->id, sizeof(f->id), "mtkfb-emu");
            f->smem_len = EMU_FB_SZ;
            f->line_length = EMU_STRIDE;
            return 0;
        }
        return 0;
    }
    if (fd == g_ev0_fd) {
        if (_IOC_NR(request) == _IOC_NR(EVIOCGNAME(0)) && arg) {
            snprintf((char *)arg, _IOC_SIZE(request), "mtk-kpd");
            return 0;
        }
        return 0;
    }
    if (fd == g_eac_fd) {
        return 0;
    }
    return real_ioctl(fd, request, arg);
}
