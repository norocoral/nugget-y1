#ifndef CONFIGAPP_CFG_REGISTRY_H
#define CONFIGAPP_CFG_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum cfg_item_type {
    CFG_TYPE_TOGGLE = 0,
    CFG_TYPE_TRISTATE,
    CFG_TYPE_CHOICE,
    CFG_TYPE_INT,
    CFG_TYPE_INFO,
    CFG_TYPE_ACTION
};

struct cfg_item {
    const char        *id;          /* Persistent key in settings.conf (NULL if transient/info) */
    const char        *label;
    const char        *label_ko;
    const char        *subtitle;
    const char        *subtitle_ko;
    const char        *icon;        /* Lucide SVG icon name */
    enum cfg_item_type type;
    int                min_val;
    int                max_val;
    int                step;
    const char        *unit;
    const char       **choices;
    const char       **choices_ko;
    int                num_choices;
    int  (*get_int)(void);
    void (*set_int)(int val);
    void (*get_str)(char *buf, size_t len);
    void (*on_action)(void);
};

struct cfg_page {
    const char      *id;
    const char      *title;
    const char      *title_ko;
    const char      *subtitle;
    const char      *subtitle_ko;
    const char      *icon;          /* Lucide SVG icon name */
    uint32_t         accent_color;
    struct cfg_item *items;
    int              num_items;
    bool             is_bt_page;    /* Enables live BT device manager view */
    bool             is_wifi_page;  /* Enables live Wi-Fi network manager view */
    bool             is_launch_page;/* Scans hidden folders for executables */
    bool             is_debug_page; /* Live scrollable debug info lines */
};

#endif /* CONFIGAPP_CFG_REGISTRY_H */
