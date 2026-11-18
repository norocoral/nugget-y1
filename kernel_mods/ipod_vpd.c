/*
 * ipod_vpd.c - Minimal standalone Linux 3.4.5 ARMv7 kernel module for Y1
 *
 * Serves Apple iPod 5.5G SCSI INQUIRY EVPD pages (0x00, 0xC0, 0xC2..0xFF)
 * containing the SysInfoExtended XML device descriptor ("Checkpoint" data)
 * required by iTunes (Windows) and Finder/Music.app (macOS).
 *
 * Built directly with arm-linux-gnueabihf-gcc -c without needing full
 * MT6572 kernel headers; ABI matches mtk_stp_wmt.ko (3.4.5 SMP preempt mod_unload ARMv7).
 */

#include <stdint.h>
#include <stddef.h>
#include "../liby1/src/ipod_sysinfo_5g.h"

#define IPOD_VPD_HOOK_ADDR  0xc05a3a70UL
#define IPOD_XML_PAGE       0xf8
#define IPOD_XML_FIRST_PAGE 0xc2
#define IPOD_XML_MAX_PAGES  (0x100 - IPOD_XML_FIRST_PAGE)
#define IPOD_XML_MAX_SIZE   (IPOD_XML_PAGE * IPOD_XML_MAX_PAGES)

/* Exported kernel symbols (resolved by insmod from kernel __ksymtab) */
extern int printk(const char *fmt, ...);
extern char param_ops_charp[];

/* Module parameters: serial=..., guid=... */
static char *serial = "8K7289LAV9K";
static char *guid   = "000A270019B48C1F";

struct kernel_param_345 {
    const char *name;
    const void *ops;
    uint16_t perm;
    int8_t level;
    uint8_t flags;
    void *arg;
};

static const char __param_str_serial[] __attribute__((section(".rodata"))) = "serial";
static const char __param_str_guid[]   __attribute__((section(".rodata"))) = "guid";

static const struct kernel_param_345 __param_serial
    __attribute__((used, section("__param"), aligned(4))) = {
    .name = __param_str_serial,
    .ops  = param_ops_charp,
    .perm = 0644,
    .level = -1,
    .flags = 0,
    .arg  = &serial,
};

static const struct kernel_param_345 __param_guid
    __attribute__((used, section("__param"), aligned(4))) = {
    .name = __param_str_guid,
    .ops  = param_ops_charp,
    .perm = 0644,
    .level = -1,
    .flags = 0,
    .arg  = &guid,
};

/* Rendered SysInfoExtended XML buffer */
static char ipod_xml[IPOD_XML_MAX_SIZE + 1];
static int ipod_xml_size;

static void render_ipod_xml(void)
{
    const char *ser = (serial && serial[0]) ? serial : "8K7289LAV9K";
    const char *fw  = (guid   && guid[0])   ? guid   : "000A270019B48C1F";
    const char *ram = "64";
    const char *t   = ipod_xml_template;
    int n = 0;

    while (*t && n < IPOD_XML_MAX_SIZE) {
        const char *val = NULL;
        if (*t == '@') {
            if (t[1] == 'S' && t[2] == 'E' && t[3] == 'R' &&
                t[4] == 'I' && t[5] == 'A' && t[6] == 'L' && t[7] == '@') {
                val = ser;
                t += 8;
            } else if (t[1] == 'G' && t[2] == 'U' && t[3] == 'I' &&
                       t[4] == 'D' && t[5] == '@') {
                val = fw;
                t += 6;
            } else if (t[1] == 'R' && t[2] == 'A' && t[3] == 'M' && t[4] == '@') {
                val = ram;
                t += 5;
            }
        }
        if (val) {
            while (*val && n < IPOD_XML_MAX_SIZE)
                ipod_xml[n++] = *val++;
        } else {
            ipod_xml[n++] = *t++;
        }
    }
    ipod_xml[n] = '\0';
    ipod_xml_size = n;
}

/*
 * Called from patched kernel do_inquiry() whenever ipod_vpd.ko is loaded.
 * Handles both standard INQUIRY (EVPD=0, returning exact space-padded
 * "Apple   iPod            1.62" so macOS & Windows both match) and
 * EVPD pages 0x00, 0xC0, 0xC2..0xFF.
 * Returns reply length >= 0 on success, or -1 for invalid/unsupported request.
 */
static int ipod_vpd_handler(const uint8_t *cmnd, uint8_t *buf, uint32_t max_len)
{
    if ((cmnd[1] & 1) == 0) {
        static const uint8_t std_inq[36] = {
            0x00, /* DIRECT_ACCESS_DEVICE */
            0x80, /* RMB = 1 (removable) */
            0x02, /* ANSI SCSI-2 */
            0x02, /* Response Data Format = 2 */
            0x1f, /* Additional Length = 31 (total 36) */
            0x00, 0x00, 0x00,
            'A','p','p','l','e',' ',' ',' ',
            'i','P','o','d',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
            '1','.','6','2'
        };
        int i;
        if (cmnd[2] != 0)
            return -1;
        int total = 36;
        if ((uint32_t)total > max_len)
            total = (int)max_len;
        for (i = 0; i < total; i++)
            buf[i] = std_inq[i];
        return total;
    }

    uint8_t page = cmnd[2];
    int pages = (ipod_xml_size + IPOD_XML_PAGE - 1) / IPOD_XML_PAGE;
    int n = 0;
    int i;

    if (pages > IPOD_XML_MAX_PAGES)
        pages = IPOD_XML_MAX_PAGES;

    if (page == 0x00 || page == 0xc0) {
        if (page == 0x00) {
            buf[4 + n++] = 0x00;
            buf[4 + n++] = 0xc0;
        }
        for (i = 0; i < pages; i++)
            buf[4 + n++] = (uint8_t)(IPOD_XML_FIRST_PAGE + i);
    } else if (page >= IPOD_XML_FIRST_PAGE && page < IPOD_XML_FIRST_PAGE + pages) {
        int offset = (page - IPOD_XML_FIRST_PAGE) * IPOD_XML_PAGE;
        int rem = ipod_xml_size - offset;
        n = (rem < IPOD_XML_PAGE) ? rem : IPOD_XML_PAGE;
        for (i = 0; i < n; i++)
            buf[4 + i] = (uint8_t)ipod_xml[offset + i];
    } else {
        return -1;
    }

    buf[0] = 0x00; /* DIRECT_ACCESS_DEVICE */
    buf[1] = page;
    buf[2] = 0x00;
    buf[3] = (uint8_t)n;

    int total = 4 + n;
    if ((uint32_t)total > max_len)
        total = (int)max_len;
    return total;
}

int init_module(void)
{
    volatile unsigned long *hook = (volatile unsigned long *)IPOD_VPD_HOOK_ADDR;
    render_ipod_xml();
    *hook = (unsigned long)ipod_vpd_handler;
    printk("<6>[ipod_vpd] Loaded (xml=%d bytes, serial=%s, guid=%s)\n",
           ipod_xml_size, serial ? serial : "", guid ? guid : "");
    return 0;
}

void cleanup_module(void)
{
    volatile unsigned long *hook = (volatile unsigned long *)IPOD_VPD_HOOK_ADDR;
    *hook = 0;
    printk("<6>[ipod_vpd] Unloaded\n");
}

/* Exact Linux 3.4.5 SMP preempt mod_unload ARMv7 module metadata */
static const char __modinfo_license[] __attribute__((used, section(".modinfo"), aligned(1))) =
    "license=GPL";
static const char __modinfo_depends[] __attribute__((used, section(".modinfo"), aligned(1))) =
    "depends=";
static const char __modinfo_vermagic[] __attribute__((used, section(".modinfo"), aligned(1))) =
    "vermagic=3.4.5 SMP preempt mod_unload ARMv7 ";

/* Exact 0x13c-byte struct module matching mtk_stp_wmt.ko */
struct module_345 {
    uint8_t pad0[0x0c];
    char name[56];
    uint8_t pad1[0xbc - (0x0c + 56)];
    int (*init)(void);
    uint8_t pad2[0x134 - (0xbc + 4)];
    void (*exit)(void);
    uint8_t pad3[0x13c - (0x134 + 4)];
};

struct module_345 __this_module
    __attribute__((used, section(".gnu.linkonce.this_module"), aligned(4))) = {
    .name = "ipod_vpd",
    .init = init_module,
    .exit = cleanup_module,
};
