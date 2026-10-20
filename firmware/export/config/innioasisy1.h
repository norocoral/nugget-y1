/*
 * Configuration for Innioasis Y1 on Minimal Hosted Linux
 * MediaTek MT6572 (Dual Cortex-A7)
 */

#define MODEL_NUMBER 125
#define MODEL_NAME   "Innioasis Y1"

#define CONFIG_PLATFORM (PLATFORM_HOSTED)
#define HAVE_FPU

/* Display */
#define HAVE_LCD_COLOR
#define HAVE_LCD_ENABLE
#define HAVE_LCD_SLEEP
#define HAVE_LCD_SLEEP_SETTING
#define HAVE_LCD_SHUTDOWN
#define HAVE_ALBUMART
#define HAVE_BMP_SCALING
#define HAVE_JPEG

#ifndef LCD_WIDTH
#define LCD_WIDTH  480
#endif
#ifndef LCD_HEIGHT
#define LCD_HEIGHT 360
#endif
#define LCD_DEPTH  16
#define LCD_PIXELFORMAT RGB565
#define LCD_DPI 165
#define CONFIG_LCD LCD_COWOND2

/* Backlight */
#define HAVE_BACKLIGHT
#define HAVE_BACKLIGHT_BRIGHTNESS
#define MIN_BRIGHTNESS_SETTING      0
#define MAX_BRIGHTNESS_SETTING      100
#define DEFAULT_BRIGHTNESS_SETTING  70
#define BRIGHTNESS_STEP             5
#define CONFIG_BACKLIGHT_FADING BACKLIGHT_FADING_SW_SETTING
#define BACKLIGHT_INNIOASIS
#define ROCKBOX_NO_TEMP_SETTINGS_FILE

/* Keypad & Input */
#define CONFIG_KEYPAD INNIOASIS_Y1_PAD
#define HAVE_SCROLLWHEEL
#define HAVE_BUTTON_DATA
#define HAVE_VOLUME_IN_LIST
#define HAVE_MULTIMEDIA_KEYS

/* Audio */
#define HAVE_SW_TONE_CONTROLS
#define HAVE_SW_VOLUME_CONTROL
#define HAVE_DUMMY_CODEC
#define HAVE_HEADPHONE_DETECTION
#define HW_SAMPR_CAPS (SAMPR_CAP_48 | SAMPR_CAP_44 | SAMPR_CAP_32 | \
                       SAMPR_CAP_24 | SAMPR_CAP_22 | SAMPR_CAP_16 | \
                       SAMPR_CAP_12 | SAMPR_CAP_11 | SAMPR_CAP_8)

/* LED / Virtual Disk Access */
#define CONFIG_LED LED_VIRTUAL

/* Real-Time Clock & Power */
#define CONFIG_RTC RTC_HOSTED
#define CONFIG_BATTERY_MEASURE PERCENTAGE_MEASURE
#define CONFIG_CHARGING CHARGING_MONITOR
#define HAVE_SW_POWEROFF
#define BATTERY_DEV_NAME "battery"
#define POWER_DEV_NAME   "ac"

/* System Features */
#define HAVE_QUICKSCREEN
#define HAVE_HOTKEY
#define HAVE_TAGCACHE

/* Buffers */
#define CODEC_SIZE 0x100000
#define PLUGIN_BUFFER_SIZE 0x200000

/* Storage */
#define CONFIG_STORAGE STORAGE_HOSTFS
#define PIVOT_ROOT "/mnt/sdcard"
#define HAVE_STORAGE_FLUSH
#define HAVE_MULTIDRIVE
#define NUM_DRIVES 2
#define HAVE_HOTSWAP
#define MULTIDRIVE_DIR "/mnt/extsd"
#define BOOTDIR "/.rockbox"


