#!/usr/bin/env python3
"""
Innioasis Y1 Build System - Interactive Terminal GUI Configurator
Allows configuring modular build components (liby1, rootfs/init, boot_nugget.img,
liby1video.so, Rockbox or standalone custom C app, SD card .system packaging).
"""

import curses
import os
import re
import sys

SCHEMA = [
    {
        "key": "BUILD_LIBY1",
        "label": "Build liby1 Hardware Layer (liby1.a / liby1.so / liby1.h)",
        "type": "bool",
        "default": "1",
        "help": "Compiles the standalone C hardware library in hosted_linux_port/liby1/.",
    },
    {
        "key": "BUILD_ROOTFS",
        "label": "Build Initramfs & Native Utilities (init, unmute_eac, splash)",
        "type": "bool",
        "default": "1",
        "help": "Populates rootfs, converts splash/bootmenu PNGs, and compiles init.",
    },
    {
        "key": "BUILD_BOOTIMG",
        "label": "Pack Android Kernel Boot Image (boot_nugget.img)",
        "type": "bool",
        "default": "1",
        "help": "Packages rootfs into ramdisk_nugget.img and wraps with MT6572 kernel.",
    },
    {
        "key": "BUILD_VIDEO_LIB",
        "label": "Build Hardware Video Helper (liby1video.so)",
        "type": "bool",
        "default": "1",
        "help": "Compiles liby1video.so and bundles FFmpeg into /.system/ on SD card.",
    },
    {
        "key": "BUILD_ROCKBOX",
        "label": "Build Rockbox Application (.rockbox/ on SD card)",
        "type": "bool",
        "default": "1",
        "help": "Disable to build a pure standalone Linux/liby1 SDK image without Rockbox.",
    },
    {
        "key": "CUSTOM_APP_BIN",
        "label": "Custom Standalone App Binary Path (-> /.system/app)",
        "type": "str",
        "default": "",
        "help": "Optional path to a custom ARM binary to install at /.system/app on SD card.",
    },
    {
        "key": "PACKAGE_SDCARD",
        "label": "Create SD Card Package (.system/ & .rockbox/ zip)",
        "type": "bool",
        "default": "1",
        "help": "Stages all system/debug/hardware files into /.system/ and zips SD package.",
    },
    {
        "key": "BUNDLE_BT_BLOBS",
        "label": "Bundle MT6572/MT6627 Bluetooth Modules & Firmware (-> /.system/bt)",
        "type": "bool",
        "default": "1",
        "help": "Includes mtk_stp_wmt.ko, mtk_stp_bt.ko, and SOC1_0_RAM_code_e1_hdr.bin.",
    },
    {
        "key": "BUNDLE_ANDROID_RAMDISK",
        "label": "Bundle Stock Android Ramdisk for Dual-Boot (-> /.system/)",
        "type": "bool",
        "default": "1",
        "help": "Copies ramdisk.cpio.gz to /.system/ramdisk_android.cpio.gz for Boot Menu.",
    },
    {
        "key": "USE_CCACHE",
        "label": "Enable ccache Compiler Acceleration",
        "type": "bool",
        "default": "1",
        "help": "Uses ccache if installed to dramatically speed up incremental C builds.",
    },
    {
        "key": "CROSS_COMPILE",
        "label": "ARM Cross-Compiler Prefix",
        "type": "str",
        "default": "arm-linux-gnueabihf-",
        "help": "Toolchain prefix used for liby1, initramfs utilities, and Rockbox.",
    },
    {
        "key": "JOBS",
        "label": "Parallel Build Jobs (0 = auto-detect CPU cores)",
        "type": "str",
        "default": "0",
        "help": "Number of parallel make jobs (-jN). Set to 0 to use nproc.",
    },
]

ACTIONS = [
    ("build", "[ Build Selected Components ]"),
    ("dist", "[ Build & Package Complete GitHub Distribution (dist/) ]"),
    ("only_liby1", "[ Quick Action: Build liby1 Only ]"),
    ("only_bootimg", "[ Quick Action: Build Rootfs & Boot Image Only ]"),
    ("sdk_no_rb", "[ Quick Action: Build Standalone SDK (No Rockbox) ]"),
    ("save", "[ Save Configuration & Exit ]"),
    ("quit", "[ Cancel / Quit Without Saving ]"),
]


def load_config(conf_path):
    cfg = {item["key"]: item["default"] for item in SCHEMA}
    if not os.path.exists(conf_path):
        return cfg
    try:
        with open(conf_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                k = k.strip()
                v = v.strip()
                if len(v) >= 2 and ((v[0] == '"' and v[-1] == '"') or (v[0] == "'" and v[-1] == "'")):
                    v = v[1:-1]
                if k in cfg:
                    cfg[k] = v
    except OSError:
        pass
    return cfg


def save_config(conf_path, cfg):
    lines = [
        "# =============================================================================",
        "# Innioasis Y1 Build Configuration (build.conf)",
        "# =============================================================================",
        "# Edit this file directly or run `./build.sh --gui` for the interactive TUI.",
        "",
    ]
    for item in SCHEMA:
        key = item["key"]
        val = cfg.get(key, item["default"])
        lines.append(f"# {item['label']}")
        lines.append(f"# {item['help']}")
        if item["type"] == "bool":
            lines.append(f"{key}={1 if val == '1' else 0}")
        else:
            escaped = str(val).replace('"', '\\"')
            lines.append(f'{key}="{escaped}"')
        lines.append("")
    with open(conf_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def prompt_string_curses(stdscr, title, current_val):
    h, w = stdscr.getmaxyx()
    win_w = min(max(60, len(title) + 10), w - 4)
    win_h = 7
    win_y = max(1, (h - win_h) // 2)
    win_x = max(2, (w - win_w) // 2)

    win = curses.newwin(win_h, win_w, win_y, win_x)
    win.keypad(True)
    win.box()
    win.addstr(1, 2, title[: win_w - 4], curses.A_BOLD)
    win.addstr(2, 2, "Enter value (ESC to cancel, ENTER to confirm):"[: win_w - 4])

    buf = list(current_val)
    curses.curs_set(1)
    while True:
        win. move(4, 2)
        win.clrtoeol()
        win.box()
        display_str = "".join(buf)
        max_field = win_w - 6
        if len(display_str) > max_field:
            display_str = display_str[-max_field:]
        win.addstr(4, 3, display_str)
        win.move(4, 3 + len(display_str))
        win.refresh()

        ch = win.getch()
        if ch in (10, 13, curses.KEY_ENTER):
            curses.curs_set(0)
            return "".join(buf)
        if ch == 27:  # ESC
            curses.curs_set(0)
            return current_val
        if ch in (curses.KEY_BACKSPACE, 127, 8):
            if buf:
                buf.pop()
        elif 32 <= ch <= 126:
            buf.append(chr(ch))


def run_curses_tui(stdscr, conf_path, cfg):
    curses.curs_set(0)
    curses.use_default_colors()
    if curses.has_colors():
        curses.init_pair(1, curses.COLOR_CYAN, -1)
        curses.init_pair(2, curses.COLOR_GREEN, -1)
        curses.init_pair(3, curses.COLOR_YELLOW, -1)
        curses.init_pair(4, curses.COLOR_BLACK, curses.COLOR_CYAN)

    total_items = len(SCHEMA) + len(ACTIONS)
    idx = 0
    status_msg = "Use UP/DOWN arrows to navigate, SPACE to toggle, ENTER to select, B to build, Q to quit."

    while True:
        stdscr.erase()
        h, w = stdscr.getmaxyx()

        title = " Innioasis Y1 Modular Build System & SDK Configurator "
        stdscr.addstr(0, max(0, (w - len(title)) // 2), title[: w - 1], curses.color_pair(1) | curses.A_BOLD)
        subtitle = " Decoupled Hardware Layer (liby1) + Rootfs/BootImg + SD (.system & .rockbox) "
        if h > 2:
            stdscr.addstr(1, max(0, (w - len(subtitle)) // 2), subtitle[: w - 1], curses.A_DIM)

        row = 3
        if row < h - 4:
            stdscr.addstr(row, 2, "Build Components & Options:", curses.A_BOLD | curses.color_pair(3))
            row += 1

        for i, item in enumerate(SCHEMA):
            if row >= h - 6:
                break
            is_sel = i == idx
            attr = curses.color_pair(4) | curses.A_BOLD if is_sel else curses.A_NORMAL
            val = cfg.get(item["key"], item["default"])
            if item["type"] == "bool":
                box_str = "[X]" if val == "1" else "[ ]"
                line = f" {box_str} {item['label']}"
            else:
                disp_val = val if val else "<none>"
                line = f"     {item['label']}: [{disp_val}]"
            stdscr.addstr(row, 2, line[: w - 4].ljust(min(w - 4, 76)), attr)
            row += 1

        row += 1
        if row < h - 4:
            stdscr.addstr(row, 2, "Actions:", curses.A_BOLD | curses.color_pair(3))
            row += 1

        for j, (act_id, act_label) in enumerate(ACTIONS):
            if row >= h - 3:
                break
            item_index = len(SCHEMA) + j
            is_sel = item_index == idx
            attr = curses.color_pair(4) | curses.A_BOLD if is_sel else curses.color_pair(2) | curses.A_BOLD
            stdscr.addstr(row, 4, f" {act_label} "[: w - 6], attr)
            row += 1

        # Help box at bottom
        if h >= 6:
            help_text = status_msg
            if idx < len(SCHEMA):
                help_text = SCHEMA[idx]["help"]
            stdscr.hline(h - 3, 1, curses.ACS_HLINE, max(1, w - 2))
            stdscr.addstr(h - 2, 2, f"Info: {help_text}"[: w - 4], curses.color_pair(1))
            stdscr.addstr(
                h - 1,
                2,
                "Keys: [Up/Down] Move  [Space] Toggle  [Enter] Select/Edit  [S] Save  [B] Build  [Q] Quit"[: w - 4],
                curses.A_DIM,
            )

        stdscr.refresh()
        ch = stdscr.getch()

        if ch in (curses.KEY_UP, ord("k"), ord("K")):
            idx = (idx - 1) % total_items
        elif ch in (curses.KEY_DOWN, ord("j"), ord("J")):
            idx = (idx + 1) % total_items
        elif ch in (ord("q"), ord("Q"), 27):
            return 2
        elif ch in (ord("s"), ord("S")):
            save_config(conf_path, cfg)
            return 2
        elif ch in (ord("b"), ord("B")):
            save_config(conf_path, cfg)
            return 0
        elif ch == ord(" ") and idx < len(SCHEMA):
            item = SCHEMA[idx]
            if item["type"] == "bool":
                cfg[item["key"]] = "0" if cfg.get(item["key"]) == "1" else "1"
            else:
                cfg[item["key"]] = prompt_string_curses(stdscr, item["label"], cfg.get(item["key"], ""))
        elif ch in (10, 13, curses.KEY_ENTER):
            if idx < len(SCHEMA):
                item = SCHEMA[idx]
                if item["type"] == "bool":
                    cfg[item["key"]] = "0" if cfg.get(item["key"]) == "1" else "1"
                else:
                    cfg[item["key"]] = prompt_string_curses(stdscr, item["label"], cfg.get(item["key"], ""))
            else:
                act_id = ACTIONS[idx - len(SCHEMA)][0]
                if act_id == "build":
                    save_config(conf_path, cfg)
                    return 0
                if act_id == "dist":
                    for it in SCHEMA:
                        if it["type"] == "bool" and (it["key"].startswith("BUILD_") or it["key"] == "PACKAGE_SDCARD"):
                            cfg[it["key"]] = "1"
                    save_config(conf_path, cfg)
                    return 3
                if act_id == "only_liby1":
                    for it in SCHEMA:
                        if it["type"] == "bool" and it["key"].startswith("BUILD_") or it["key"] == "PACKAGE_SDCARD":
                            cfg[it["key"]] = "0"
                    cfg["BUILD_LIBY1"] = "1"
                    save_config(conf_path, cfg)
                    return 0
                if act_id == "only_bootimg":
                    cfg["BUILD_LIBY1"] = "1"
                    cfg["BUILD_ROOTFS"] = "1"
                    cfg["BUILD_BOOTIMG"] = "1"
                    cfg["BUILD_VIDEO_LIB"] = "0"
                    cfg["BUILD_ROCKBOX"] = "0"
                    cfg["PACKAGE_SDCARD"] = "0"
                    save_config(conf_path, cfg)
                    return 0
                if act_id == "sdk_no_rb":
                    cfg["BUILD_LIBY1"] = "1"
                    cfg["BUILD_ROOTFS"] = "1"
                    cfg["BUILD_BOOTIMG"] = "1"
                    cfg["BUILD_VIDEO_LIB"] = "1"
                    cfg["BUILD_ROCKBOX"] = "0"
                    cfg["PACKAGE_SDCARD"] = "1"
                    save_config(conf_path, cfg)
                    return 0
                if act_id == "save":
                    save_config(conf_path, cfg)
                    return 2
                if act_id == "quit":
                    return 2


def run_fallback_cli(conf_path, cfg):
    while True:
        print("\n=====================================================================")
        print("  Innioasis Y1 Modular Build System & SDK Configurator")
        print("=====================================================================")
        for i, item in enumerate(SCHEMA, 1):
            val = cfg.get(item["key"], item["default"])
            if item["type"] == "bool":
                mark = "[X]" if val == "1" else "[ ]"
                print(f"  {i:2d}. {mark} {item['label']}")
            else:
                disp = val if val else "<none>"
                print(f"  {i:2d}.     {item['label']}: [{disp}]")
        print("---------------------------------------------------------------------")
        print("   B. Save & Run Build")
        print("   D. Build & Package Complete GitHub Distribution (dist/)")
        print("   L. Quick Build: liby1 Only")
        print("   K. Quick Build: Standalone Y1 SDK & Boot Image (No Rockbox)")
        print("   S. Save Configuration & Exit")
        print("   Q. Cancel / Quit")
        print("=====================================================================")
        try:
            choice = input("Select item number to toggle/edit, or action [B/L/K/S/Q]: ").strip()
        except EOFError:
            return 2

        if not choice:
            continue
        u = choice.upper()
        if u == "B":
            save_config(conf_path, cfg)
            return 0
        if u == "D":
            for it in SCHEMA:
                if it["type"] == "bool" and (it["key"].startswith("BUILD_") or it["key"] == "PACKAGE_SDCARD"):
                    cfg[it["key"]] = "1"
            save_config(conf_path, cfg)
            return 3
        if u == "L":
            for it in SCHEMA:
                if it["type"] == "bool" and (it["key"].startswith("BUILD_") or it["key"] == "PACKAGE_SDCARD"):
                    cfg[it["key"]] = "0"
            cfg["BUILD_LIBY1"] = "1"
            save_config(conf_path, cfg)
            return 0
        if u == "K":
            cfg["BUILD_LIBY1"] = "1"
            cfg["BUILD_ROOTFS"] = "1"
            cfg["BUILD_BOOTIMG"] = "1"
            cfg["BUILD_ROCKBOX"] = "0"
            cfg["PACKAGE_SDCARD"] = "1"
            save_config(conf_path, cfg)
            return 0
        if u == "S":
            save_config(conf_path, cfg)
            print(f"[*] Saved configuration to {conf_path}")
            return 2
        if u == "Q":
            return 2
        if choice.isdigit():
            num = int(choice)
            if 1 <= num <= len(SCHEMA):
                item = SCHEMA[num - 1]
                if item["type"] == "bool":
                    cfg[item["key"]] = "0" if cfg.get(item["key"]) == "1" else "1"
                else:
                    new_val = input(f"Enter new value for {item['label']} [{cfg.get(item['key'], '')}]: ").strip()
                    cfg[item["key"]] = new_val


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_conf = os.path.abspath(os.path.join(script_dir, "..", "build.conf"))
    conf_path = sys.argv[1] if len(sys.argv) > 1 else default_conf

    cfg = load_config(conf_path)
    if sys.stdin.isatty() and sys.stdout.isatty():
        try:
            rc = curses.wrapper(run_curses_tui, conf_path, cfg)
            sys.exit(rc)
        except curses.error:
            rc = run_fallback_cli(conf_path, cfg)
            sys.exit(rc)
    else:
        rc = run_fallback_cli(conf_path, cfg)
        sys.exit(rc)


if __name__ == "__main__":
    main()

