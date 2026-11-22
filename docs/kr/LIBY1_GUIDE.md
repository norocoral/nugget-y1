🌐 **한국어** | [English](../LIBY1_GUIDE.md)

# `liby1` 개발자 가이드 및 API 레퍼런스

`liby1`은 다운스트림 리눅스 커널 3.4.67 기반의 Innioasis Y1(MediaTek MT6572 SoC, MT6323 PMIC, MT6627 무선/FM 콤보)용 독립형 C 하드웨어 추상화 라이브러리입니다. 프레임버퍼 디스플레이, `/dev/eac` 오디오 엔진, `/dev/input/event*` 입력 장치, `/dev/fm` 튜너, CPU/핫플러그 sysfs 제어, wakelock/절전 상태, USB 가젯 sysfs 제어 및 `iTunesDB`/`ArtworkDB` 파서를 제공합니다.

모든 공개 선언은 [`liby1.h`](../../liby1/include/liby1.h)에 정의되어 있습니다.

---

## 1. 대상 플랫폼 및 ABI

| 항목 | 사양 |
| :--- | :--- |
| SoC | MediaTek MT6572 (듀얼 코어 ARM Cortex-A7, 598 MHz – 1300 MHz) |
| ABI / 타겟 트리플 | `arm-linux-gnueabihf` (ARMv7-A, VFPv4, NEON, hard-float) |
| 메모리 | 512 MB LPDDR2 |
| 디스플레이 | `/dev/graphics/fb0` 480x360 LCD (하드웨어 오버레이 XRGB8888, 소프트웨어 RGB565 출력 지원) |
| 오디오 | MT6572 AFE (`/dev/eac`, 32 KB DL1 DMA, 16-bit 스테레오 인터리브) + MT6323 PMIC DAC/PGA/ADC |
| 입력 | 정전식 터치 휠 + 5개 물리 버튼 + 3.5mm TRRS `h2w`/`accdet` |
| 연결성 | USB 2.0 High-Speed (`android_usb` 대용량 저장소, ACM 시리얼 `/dev/ttyGS0`–`/dev/ttyGS1`), Bluetooth 4.0 (MT6627), FM 라디오 (`/dev/fm`) |
| 스토리지 | MicroSD (`/mnt/sdcard`, FAT32/exFAT/ext4) |

---

## 2. 부트로더 연동 및 실행 환경

애플리케이션은 부트로더 메뉴(`Pick 1`–`Pick 3`) 또는 기본 폴백 바이너리인 `/.system/app`을 통해 실행됩니다. PID 1 `/init`은 `exec()` 호출 전에 다음 환경을 설정합니다:

- 작업 디렉터리: `/mnt/sdcard` (또는 애플리케이션 디렉터리)
- `PATH=/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sdcard/.system/bin:/mnt/sdcard/.rockbox/bin:/mnt/sdcard/.meui/bin:/mnt/sdcard/.custom/bin`
- `LD_LIBRARY_PATH=/mnt/sdcard/.meui/lib:/mnt/sdcard/.custom/lib:/mnt/sdcard/.system/lib:/mnt/sdcard/.rockbox/lib:/lib:/usr/lib`
- `HOME=/mnt/sdcard`
- `TERM=linux`

`LD_LIBRARY_PATH`에 `/mnt/sdcard/.custom/lib`과 `/mnt/sdcard/.system/lib`이 포함되어 있으므로 동적 링크 바이너리는 루트 파일시스템을 수정하지 않고 `liby1.so`를 링크할 수 있으며, `liby1.a` 정적 링크도 지원됩니다.

### SD 카드 디렉터리 구조

```text
/mnt/sdcard/
├── .system/                      # 공용 시스템 디렉터리 (/.system으로 심볼릭 링크)
│   ├── include/liby1.h           # 공개 C 헤더
│   ├── lib/liby1.a               # 정적 라이브러리
│   ├── lib/liby1.so              # 공유 라이브러리
│   ├── lib/liby1video.so         # 비디오 디코더 라이브러리
│   ├── bin/                      # 보조 바이너리
│   ├── bt/                       # MediaTek 블루투스 모듈 및 펌웨어
│   ├── y1_debug.txt              # 디버그 로그 출력 (y1_log_set_enabled(true) 시)
│   └── app                       # 선택적 기본 자동 부팅 바이너리
├── .custom/                      # 부트로더 Pick 3
│   ├── launch                    # ELF 바이너리 또는 chmod +x 셸 스크립트
│   ├── bin/
│   └── lib/
├── .meui/                        # 부트로더 Pick 2
│   ├── meui
│   └── lib/
└── .rockbox/                     # 부트로더 Pick 1
    └── rockbox
```

---

## 3. 서브시스템 가이드 및 API 레퍼런스

### 3.1 코어, Sysfs, 로깅, 전원 및 배터리

`y1_init()`은 다음 순서로 핵심 런타임을 초기화합니다: `/mnt/sdcard/.system` 디렉터리 생성 확인, `/dev/graphics/fb0` 열기 및 매핑(`y1_fb_init()`), 백라이트 밝기 50% 설정, 터치 휠 스캔 활성화(`y1_input_set_touch_scanning(true)`), 모든 `/dev/input/event*` 디바이스 열기(`y1_input_init()`). `/dev/eac`(오디오)와 `/dev/fm`(라디오)은 자동으로 열지 않으므로 필요한 경우에만 별도로 초기화해야 합니다.

`y1_shutdown()`은 오디오 종료, FM 튜너 전원 차단, 입력 파일 디스크립터 닫기, 프레임버퍼 닫기, 로그 파일 디스크립터 닫기 순으로 정리합니다. `main()` 종료 전 호출하여 부트로더나 다음 프로세스가 깨끗한 하드웨어 상태를 이어받도록 해야 합니다.

```c
int  y1_init(void);
void y1_shutdown(void);

bool y1_sysfs_write(const char *path, const char *val);
bool y1_sysfs_write_int(const char *path, int val);
bool y1_sysfs_read(const char *path, char *buf, size_t size);
bool y1_sysfs_read_int(const char *path, int *out_val);

void y1_log_set_enabled(bool enabled);
bool y1_log_is_enabled(void);
void y1_log_raw(const char *msg, size_t len);
void y1_log(const char *msg);
void y1_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int  y1_battery_percent(void);       /* 0..100 */
int  y1_battery_voltage_mv(void);    /* 밀리볼트(mV) */
int  y1_battery_current_ma(void);    /* 밀리암페어(mA) */
bool y1_battery_is_charging(void);
bool y1_power_usb_online(void);

void y1_sys_poweroff(void) __attribute__((noreturn));
void y1_sys_reboot(void) __attribute__((noreturn));
```

- **Sysfs 헬퍼:** `y1_sysfs_read()`는 끝부분의 `\r`/`\n`을 제거하고 NUL 문자로 종료합니다. 읽기/쓰기 헬퍼 모두 `O_CLOEXEC`로 열고 즉시 닫으며, 열기 또는 I/O 오류 시 `false`를 반환합니다.
- **로깅:** 기본값은 비활성화입니다. `y1_log_set_enabled(true)` 호출 시 `stdout`, `/dev/kmsg`, `/mnt/sdcard/.system/y1_debug.txt`에 동시에 출력합니다.
- **배터리 및 전원:** `y1_battery_*()`는 MT6323 전원 공급 장치 sysfs 노드(`/sys/class/power_supply/battery/`)를 읽습니다. `y1_sys_poweroff()` 및 `y1_sys_reboot()`는 `sync()` 실행 후 커널 전원 종료/재부팅 시스템 콜을 호출합니다.

---

### 3.2 디스플레이, 프레임버퍼, 백라이트 및 절전

Y1 패널은 480x360 해상도이며 `/dev/graphics/fb0`은 32-bit `XRGB8888` 포맷으로 동작합니다. 임베디드 UI나 레트로 포트가 주로 16-bit `RGB565`를 사용하므로, `liby1`은 사전 계산된 RGB565-to-XRGB8888 룩업 테이블을 통해 소프트웨어 RGB565 버퍼를 하드웨어 프레임버퍼에 직접 전송합니다.

모든 출력 함수(`y1_fb_present_rgb565()`, `y1_fb_present_xrgb8888()`, `y1_fb_blit_rgb565_rect()`, `y1_fb_blit_rgb565_classic_scaled()`)는 매핑된 프레임버퍼에 기록한 뒤 자동으로 `y1_fb_flip()`(`FBIOPAN_DISPLAY`)을 호출하여 MT6572 디스플레이 오버레이를 갱신합니다. `y1_fb_mmap_ptr()`로 직접 메모리에 쓰는 경우에는 버퍼 갱신 후 `y1_fb_flip()`을 직접 호출해야 합니다.

```c
#define Y1_FB_WIDTH         480
#define Y1_FB_HEIGHT        360
#define Y1_CLASSIC_WIDTH    320
#define Y1_CLASSIC_HEIGHT   240

typedef uint16_t (*y1_pixel_blend_fn)(int x, int y, uint16_t cur_px, void *user);

int      y1_fb_init(void);
void     y1_fb_close(void);
bool     y1_fb_is_ready(void);

int      y1_fb_width(void);
int      y1_fb_height(void);
int      y1_fb_bpp(void);
int      y1_fb_stride(void);
uint8_t *y1_fb_mmap_ptr(void);

void     y1_fb_flip(void);
void     y1_fb_set_eco_mode(bool eco_on);
uint32_t y1_fb_rgb565_to_xrgb8888(uint16_t px);

void y1_fb_blit_rgb565_rect(const uint16_t *src_base, int src_stride_px,
                            int x, int y, int width, int height,
                            y1_pixel_blend_fn blend_cb, void *blend_user);

void y1_fb_blit_rgb565_classic_scaled(const uint16_t *src_base, int src_stride_px,
                                      int x, int y, int width, int height,
                                      y1_pixel_blend_fn blend_cb, void *blend_user);

void y1_fb_present_rgb565(const uint16_t *pixels_480x360);
void y1_fb_present_xrgb8888(const uint32_t *pixels_480x360);

bool y1_fb_display_raw_screen(const char *name, uint16_t *out_fb16,
                              int fb16_stride_px, bool classic_320x240);

void y1_backlight_set_raw(int val_0_to_255);
void y1_backlight_set_percent(int pct_0_to_100);
int  y1_backlight_get_raw(void);

void y1_display_enter_early_suspend(void);
void y1_display_exit_early_suspend(void);
bool y1_display_is_early_suspended(void);
void y1_power_set_idle_sleep(bool allow_suspend);
```

#### 스케일링 및 스플래시 화면
- **부분 갱신:** `y1_fb_blit_rgb565_rect()`는 행 픽셀 너비가 `src_stride_px`인 버퍼에서 `(x, y, width, height)` 영역만 전송합니다. 픽셀 단위 전환 블렌딩이 필요하지 않다면 `blend_cb`에 `NULL`을 전달합니다.
- **320x240 클래식 스케일링:** `y1_fb_blit_rgb565_classic_scaled()`는 `2x2 -> 3x3` 영역 평균 방식(정확히 1.5배 비율)으로 320x240 RGB565 버퍼를 480x360으로 확대합니다.
- **원시 BGRA 화면:** `y1_fb_display_raw_screen("boot", ...)`은 `/.system/<name>.raw`, `/mnt/sdcard/.system/<name>.raw`, `/.rockbox/<name>.raw`, `/etc/<name>.raw`에서 `480x360x4` BGRA 이미지를 찾습니다. `out_fb16`이 `NULL`이 아니면 RGB565 변환 복사본도 함께 기록합니다.

#### 디스플레이 전원 및 커널 절전 수명 주기
MT6572 커널은 안드로이드 `earlysuspend`와 `wakelock`을 사용합니다:
1. **백그라운드 동작 중 화면 끄기** (예: 화면 꺼짐 오디오 재생): `y1_display_enter_early_suspend()`를 호출합니다. `/sys/power/wake_lock`에 `"rockbox_active"`를 획득한 상태에서 `/sys/power/state`에 `"mem\n"`을 기록하므로 유저스페이스 스레드를 동결하지 않고 커널 `earlysuspend` 훅(LCD 클록, 백라이트, 터치 스캔, GPU 전원 차단)만 실행합니다.
2. **유휴 시 전체 `mem` 절전:** `y1_power_set_idle_sleep(true)`를 호출합니다. USB 대용량 저장소가 연결되어 있지 않은 경우 `"rockbox_active"` 및 `"y1play"` wakelock을 해제하고 `/sys/power/state`에 `"mem\n"`을 기록하여 커널이 RAM 절전(`mem` suspend)에 진입하도록 합니다.
3. **절전 해제:** 웨이크 입력이 감지되면 즉시 `y1_power_set_idle_sleep(false)`를 호출하여 `"rockbox_active"`를 다시 획득(커널 재절전 방지)한 뒤, `y1_display_exit_early_suspend()`(`/sys/power/state`에 `"on\n"` 기록 및 디스플레이 복구)와 백라이트 밝기 복원을 수행합니다.

---

### 3.3 CPU 주파수 및 코어 핫플러그

MT6572는 두 개의 Cortex-A7 코어(`cpu0`, `cpu1`)를 탑재하고 있습니다. `cpu1`을 온라인 상태로 유지하거나 고클록을 고정하면 유휴 및 재생 소비 전력이 증가하며, 반대로 `598 MHz` 단일 코어로 고정하면 소프트웨어 렌더러 프레임 저하 또는 대용량 DB 파싱 지연이 발생할 수 있습니다.

```c
enum y1_cpu_mode_id {
    Y1_CPU_MODE_DYNAMIC     = 0, /* Ondemand 598 MHz - 1001 MHz */
    Y1_CPU_MODE_POWERSAVE   = 1, /* 고정 598 MHz */
    Y1_CPU_MODE_BALANCED    = 2, /* 고정 1001 MHz */
    Y1_CPU_MODE_PERFORMANCE = 3, /* 고정 1196 MHz */
    Y1_CPU_MODE_TURBO       = 4  /* 고정 1300 MHz */
};

void y1_cpu_set_mode(int mode);
int  y1_cpu_get_mode(void);
void y1_cpu_set_dual_core(bool dual_core);
bool y1_cpu_is_dual_core(void);
int  y1_cpu_get_freq_mhz(void);
void y1_cpu_set_screen_off_clamp(bool screen_off);
void y1_cpu_boost_for_conversion(bool enable);
```

- `y1_cpu_set_mode()`는 `cpu0`의 `cpufreq` 거버너와 목표 주파수를 설정합니다.
- `y1_cpu_set_dual_core(false)`는 `/sys/devices/system/cpu/cpu1/online`에 `0`을 기록하여 `cpu1` 전원을 차단하고, `true`는 `cpu1`을 다시 활성화합니다.
- `y1_cpu_set_screen_off_clamp(true)`는 디스플레이가 꺼질 때 주파수를 절전 모드로 제한하고 `cpu1`을 오프라인하며, 화면 복귀 시 `false`를 전달해 이전 프로필을 복원합니다.
- `y1_cpu_boost_for_conversion(true)`는 짧은 대량 연산(`iTunesDB` 로드, 앨범아트 디코딩 등) 동안 클록과 `cpu1`을 일시 상향하며, `false` 시 이전 모드로 되돌립니다.

---

### 3.4 입력 및 3.5mm 잭

물리 버튼, 정전식 클릭 휠 회전, 3.5mm 헤드셋 인라인 버튼은 여러 `/dev/input/event*` 노드(`mtk-kpd`, `tpd`/터치 휠, `ACCDET`)를 통해 리눅스 `EV_KEY` 이벤트로 전달됩니다. `y1_input_init()`은 해당 장치들을 논블로킹 모드로 스캔하여 열고, 어떤 파일 디스크립터가 `ACCDET` 장치인지 기록해 둡니다.

```c
#define Y1_BTN_NONE         0x0000
#define Y1_BTN_SELECT       0x0001
#define Y1_BTN_MENU         0x0002
#define Y1_BTN_LEFT         0x0004
#define Y1_BTN_RIGHT        0x0008
#define Y1_BTN_SCROLL_FWD   0x0010
#define Y1_BTN_SCROLL_BACK  0x0020
#define Y1_BTN_PLAY         0x0040
#define Y1_BTN_HEADSET_MFB  0x0080
#define Y1_BTN_HEADSET_LONG 0x0100

struct y1_input_event {
    int      keycode;     /* 원시 Linux input_event.code */
    int      value;       /* 1 = 누름, 0 = 뗌, 2 = 반복 */
    uint32_t button;      /* 매핑된 Y1_BTN_* 마스크 */
    bool     is_accdet;   /* 헤드셋 ACCDET 장치 입력 여부 */
};

int      y1_input_init(void);
void     y1_input_close(void);
int      y1_input_poll(struct y1_input_event *out_ev, int timeout_ms);
uint32_t y1_input_map_keycode(int keycode);
void     y1_input_set_touch_scanning(bool enabled);

int  y1_jack_get_raw_state(void); /* /sys/class/switch/h2w/state: 0=없음, 1=4극 마이크, 2=3극 */
bool y1_jack_is_inserted(void);
bool y1_jack_has_mic(void);
```

#### 폴링 동작 및 하드웨어 특성
- **`y1_input_poll()` 반환값:** `EV_KEY` 이벤트를 읽어 `out_ev`에 채우면 `1`, 키 이벤트 없이 `timeout_ms`가 만료되면 `0`, 열린 입력 장치가 없으면 `-1`을 반환합니다. 60 FPS 루프에서는 `timeout_ms = 0`, 이벤트 대기 시에는 `-1`을 사용합니다.
- **프레임당 큐 비우기:** 빠른 휠 회전 시 한 프레임 내에 여러 개의 `Y1_BTN_SCROLL_FWD` / `Y1_BTN_SCROLL_BACK` 이벤트가 쌓이므로, 프레임당 1회만 읽지 말고 `while (y1_input_poll(&ev, 0) > 0)` 루프로 모두 소진해야 합니다.
- **휠 누름/뗌 쌍:** 정전식 휠 드라이버는 틱마다 `value = 1` 직후 `value = 0`을 즉시 전송합니다. 스크롤 틱 처리 시 `ev.value == 1`(또는 반복 포함 시 `ev.value != 0`)만 처리하고 `ev.value == 0`은 무시하십시오.
- **ACCDET 리매핑:** 일부 3.5mm 헤드셋 인라인 리모컨은 `KEY_PLAYPAUSE`를 전송하며, `y1_input_map_keycode()`는 이를 `Y1_BTN_PLAY`로 매핑합니다. 이벤트 출처가 `ACCDET`(`is_accdet == true`)인 경우 `y1_input_poll()`이 자동으로 `Y1_BTN_PLAY`를 `Y1_BTN_HEADSET_MFB`로 변환하므로 기기 본체의 Play 버튼과 구분할 수 있습니다.
- **터치 스캔 절전:** 화면이 꺼지고 휠 입력이 불필요할 때 `y1_input_set_touch_scanning(false)`를 호출하면 `/proc/tpd_keys_enable`을 끌 수 있습니다.
- **헤드폰 잭 상태:** `y1_jack_get_raw_state()`는 `/sys/class/switch/h2w/state`를 읽습니다(`0` = 미연결, `1` = 마이크 포함 4극 TRRS, `2` = 마이크 없는 3극 TRS). `y1_jack_is_inserted()`는 `1` 또는 `2`일 때 `true`, `y1_jack_has_mic()`는 `1`일 때만 `true`입니다.

```c
struct y1_input_event ev;
while (y1_input_poll(&ev, 0) > 0) {
    if (ev.value == 1) { /* 키 누름 */
        switch (ev.button) {
        case Y1_BTN_SCROLL_FWD:  /* 시계 방향 틱 */ break;
        case Y1_BTN_SCROLL_BACK: /* 반시계 방향 틱 */ break;
        case Y1_BTN_SELECT:      /* 센터 버튼 */ break;
        case Y1_BTN_MENU:        /* 상단 메뉴 버튼 */ break;
        case Y1_BTN_HEADSET_MFB: /* 3.5mm 인라인 리모컨 */ break;
        }
    }
}
```

---

### 3.5 오디오: `/dev/eac` AFE 및 MT6323 PMIC

다운스트림 미디어텍 커널 3.4.67은 표준 ALSA PCM 재생 노드를 제공하지 않습니다. 대신 디지털 오디오는 Enhanced Audio Controller 문자 디바이스(`/dev/eac`)를 통해 MT6572 Audio Frontend(AFE)의 32 KB DL1 DMA 링 버퍼로 전달되며, I2S를 거쳐 MT6323 PMIC 스테레오 DAC 및 헤드폰 앰프로 출력됩니다.

`liby1`은 `/dev/eac` ioctl, 클록 분주기, MT6323 아날로그 레지스터 및 DMA 프레임 정렬을 처리합니다.

```c
enum y1_audio_rec_source {
    Y1_REC_SOURCE_INTERNAL_MIC  = 0,
    Y1_REC_SOURCE_HEADPHONE_MIC = 1,
    Y1_REC_SOURCE_LINE_FM       = 2
};

int           y1_audio_open(unsigned long sample_rate);
void          y1_audio_close(void);
bool          y1_audio_is_open(void);
int           y1_audio_get_fd(void);

bool          y1_audio_set_sample_rate(unsigned long sample_rate);
unsigned long y1_audio_get_sample_rate(void);

void    y1_audio_set_hp_analog_gain(int idx_l, int idx_r);
void    y1_audio_set_dma_running(bool running);
ssize_t y1_audio_write_pcm(const void *pcm_stereo_s16, size_t bytes);

bool y1_audio_set_deep_idle(bool allow_cpu_powerdown);
void y1_audio_wake_lock(bool hold);

void    y1_audio_enable_recording_path(enum y1_audio_rec_source source);
void    y1_audio_disable_recording_path(void);
void    y1_audio_set_recording_pga_db(int gain_db);
bool    y1_audio_rec_start(enum y1_audio_rec_source source, unsigned long sample_rate);
ssize_t y1_audio_rec_read(void *pcm_stereo_s16, size_t bytes);
void    y1_audio_rec_stop(void);

void y1_audio_set_fm_route(bool enable);

int  y1_pmic_read_reg(uint32_t offset);
int  y1_pmic_write_reg(uint32_t offset, uint32_t val, uint32_t mask);
int  y1_afe_write_reg(uint32_t offset, uint32_t val, uint32_t mask);
void y1_pmic_tune_accdet_debounce(void);
```

#### 재생 수명 주기 및 하드웨어 제약 사항
1. **열기:** 지원 샘플레이트(`8000`, `11025`, `12000`, `16000`, `22050`, `24000`, `32000`, `44100`, `48000`) 중 하나로 `y1_audio_open(sample_rate)`를 호출합니다. 성공 시 `0`, 실패 시 음수를 반환하며, AFE 클록 설정, 32 KB DL1 DMA 버퍼 할당, MT6323 헤드폰 앰프 전원 인가가 수행됩니다.
2. **DMA 언더런 및 절전 방지:** PCM 스트리밍 중에는 반드시 다음 두 가지를 유지해야 합니다:
   - `y1_audio_wake_lock(true)`로 `"y1play"` wakelock을 유지하여 화면이 꺼질 때 커널이 `mem` 절전에 진입하지 않도록 합니다.
   - `y1_audio_set_deep_idle(false)`로 DMA 인터럽트 사이에 MT6572 SPM이 CPU/버스 전원을 차단하지 않도록 합니다.
3. **PCM 프레임 쓰기:** `y1_audio_write_pcm()`은 16-bit 부호 있는(`int16_t`) 인터리브 스테레오(`L, R`) 샘플(스테레오 프레임당 4바이트)을 입력받습니다. `/dev/eac`가 4의 배수가 아닌 바이트 수만 부분 수용한 경우, `liby1`이 4바이트 경계까지 패딩을 채워 좌/우 채널 반전이나 위상 밀림을 방지합니다.
4. **아날로그 게인:** `y1_audio_set_hp_analog_gain(idx_l, idx_r)`는 인덱스를 `0..7`(스텝당 2 dB)로 제한하여 팝 노이즈를 방지하는 헤드폰 mux/전원 비트 `[4:0]`(`0x0014`)을 유지한 채 `MT6323_AUDTOP_CON5`(`0x70a`)의 비트 `[14:12]`(L) 및 `[10:8]`(R)에 기록합니다. `y1_audio_open()` 전후 언제든 호출할 수 있으며(마지막 인덱스를 캐시했다가 오픈 시 적용),
5. **샘플레이트 전환:** 트랙 전환 시 `y1_audio_set_sample_rate(new_rate)`를 호출합니다. 현재 레이트와 같으면 하드웨어를 건드리지 않고 즉시 `true`를 반환합니다. 다른 경우 PMIC 다운링크/마스터를 게이팅하고 DL1 링 버퍼 포인터를 초기화한 뒤 AFE/PMIC 분주기를 재설정하고 CDC 클록 동기화 비트를 토글합니다.
6. **일시정지 vs 종료:** 재오픈 지연 없이 짧게 일시정지하려면 `y1_audio_set_dma_running(false)`로 헤드폰 앰프 전원을 유지한 채 AFE DL1 DMA만 멈춥니다. 재생이 완전히 중지되면 `y1_audio_set_deep_idle(true)`, `y1_audio_wake_lock(false)`, `y1_audio_close()`를 호출합니다.

```c
if (y1_audio_open(44100) == 0) {
    y1_audio_set_hp_analog_gain(6, 6);
    y1_audio_wake_lock(true);
    y1_audio_set_deep_idle(false);

    while (playing) {
        /* stereo_buf에 1024개의 인터리브 L/R int16_t 프레임 채우기 */
        y1_audio_write_pcm(stereo_buf, sizeof(stereo_buf));
    }

    y1_audio_set_deep_idle(true);
    y1_audio_wake_lock(false);
    y1_audio_close();
}
```

#### 녹음 및 FM 오디오 라우팅
- **업링크 PCM 녹음:** `y1_audio_rec_start(source, sample_rate)`(MT6323 아날로그 mux/마이크 바이어스 및 AFE 업링크 활성화)를 호출한 뒤 `y1_audio_rec_read()` 루프로 16-bit 스테레오 프레임을 읽고, `y1_audio_set_recording_pga_db(gain_db)`로 프리앰프 게인을 조절하며, 종료 시 `y1_audio_rec_stop()`을 호출합니다.
- **FM 라디오 오디오:** FM 튜너 전원을 켠 후 `y1_audio_set_fm_route(true)`를 호출하면 MT6627 I2S/MRG 오디오가 MT6572 AFE 다운링크를 거쳐 헤드폰으로 출력됩니다.
- **직접 레지스터 접근:** `y1_pmic_read_reg()`, `y1_pmic_write_reg()`, `y1_afe_write_reg()`는 `mask`를 이용한 read-modify-write 방식으로 `/dev/eac` 레지스터 ioctl을 수행합니다. `y1_pmic_tune_accdet_debounce()`는 3.5mm 헤드셋 MFB 클릭 감지를 위한 `ACCDET` PMIC 디바운스 타이밍을 조정합니다.

---

### 3.6 FM 라디오 튜너 (`/dev/fm`)

MT6627 FM 수신기는 `/dev/fm`(폴백 `/dev/radio0`, `/dev/stpfm`)을 통해 제어됩니다. 주파수는 **Hz** 단위(예: 87.5 MHz = `87500000`, 108.0 MHz = `108000000`)로 전달하며, `liby1`이 `mtk_fm_drv.ko`가 요구하는 100 kHz 단위로 변환합니다.

```c
int  y1_fm_power(bool on);
int  y1_fm_tune(int freq_hz); /* 87500000 ~ 108000000 */
int  y1_fm_seek(int freq_hz, bool up);
int  y1_fm_get_freq(void);
int  y1_fm_set_mute(bool mute);
bool y1_fm_is_muted(void);
int  y1_fm_get_rssi(void);
bool y1_fm_is_tuned(void);
int  y1_fm_set_mono(bool mono);
bool y1_fm_is_stereo(void);
```

- 일반 시작 순서: `y1_fm_power(true)` -> `y1_audio_open(44100)` (미오픈 시) -> `y1_audio_set_fm_route(true)` -> `y1_fm_tune(98500000)` -> `y1_fm_set_mute(false)`.
- `y1_fm_seek(start_freq_hz, up)`은 `start_freq_hz`에서 위(`up = true`) 또는 아래(`up = false`)로 탐색하여 고정된 주파수(Hz)를 반환합니다(실패 시 음수).
- 종료 시 음소거 및 `y1_audio_set_fm_route(false)` 호출 후 `y1_fm_power(false)`를 호출합니다.

---

### 3.7 USB 대용량 저장소 가젯

`liby1`은 `/sys/class/android_usb/android0`을 통해 MicroSD 카드(`/dev/block/mmcblk1`)를 호스트 PC에 마운트합니다.

```c
bool y1_usb_attach_mass_storage(void);
void y1_usb_detach_mass_storage(void);
bool y1_usb_is_storage_attached(void);
bool y1_usb_check_host_ejected(void);
bool y1_usb_is_serial_active(void);
void y1_usb_tune_performance(bool enable);
```

- **연결 / 해제:** USB 대용량 저장소 모드 진입 시 `y1_usb_attach_mass_storage()`를 호출합니다. 호스트 쓰기로 파일시스템 메타데이터가 손상되지 않도록 연결 전에 SD 카드의 열린 파일을 모두 플러시/닫아야 합니다. 연결 중에는 USB 가젯 유지를 위해 `y1_power_set_idle_sleep(true)`가 동작하지 않습니다.
- **호스트 안전 제거 감지:** 연결 상태에서 `y1_usb_check_host_ejected()`를 주기적으로 폴링하고, 호스트 OS가 볼륨을 안전하게 제거하면 `y1_usb_detach_mass_storage()`를 호출합니다.
- **I/O 튜닝:** `y1_usb_tune_performance(true)`는 `/sys/block/mmcblk1/queue/scheduler`를 `noop`/`deadline`으로 전환하고 `read_ahead_kb`를 `2048`, `nr_requests`를 `512`/`256`으로 설정하며 USB CPU 클록을 상향합니다. `false` 호출 시(해제 시 자동 호출) 저장해 둔 스케줄러와 큐 파라미터를 복원합니다.

---

### 3.8 iPod USB 모드 및 네이티브 `iTunesDB` API

이 서브시스템은 USB 장치 식별자 스푸핑(`y1_usb_set_ipod_mode`)과 기존 `/mnt/sdcard/iPod_Control/` 디렉터리의 직접 메모리 읽기/쓰기(`y1_ipod_native_*`) 두 부분으로 구성됩니다.

```c
void y1_usb_set_ipod_mode(bool enabled);
bool y1_usb_get_ipod_mode(void);

bool y1_ipod_ensure_device_files(void);

bool y1_ipod_native_is_active(void);
void y1_ipod_native_invalidate(void);

bool y1_ipod_native_load_ramcache(struct y1_ipod_ramcache_data *out);
void y1_ipod_native_free_ramcache(struct y1_ipod_ramcache_data *data);

bool y1_ipod_native_record_play(const char *rb_path, uint32_t play_delta,
                                uint32_t unix_time, uint32_t bookmark_ms);
bool y1_ipod_native_record_rating(const char *rb_path, int rating_rb);
bool y1_ipod_native_record_favorite(const char *rb_path, bool is_favorite);

bool y1_ipod_native_has_albumart(const char *rb_path);
bool y1_ipod_native_load_albumart_rgb565(const char *rb_path,
                                          uint16_t **out_rgb565,
                                          int *out_w, int *out_h);

int         y1_ipod_native_playlist_count(void);
const char *y1_ipod_native_playlist_name(int pl_idx);
int         y1_ipod_native_playlist_track_count(int pl_idx);
const char *y1_ipod_native_playlist_track_path(int pl_idx, int tr_idx);
```

- **USB 스푸핑 (기본 비활성화):** `y1_usb_attach_mass_storage()` 호출 전에 `y1_usb_set_ipod_mode(true)`를 설정하면 `liby1`이 `ipod_vpd.ko`(SCSI EVPD 스푸핑)를 로드하고 Apple iPod 5.5G USB 디스크립터(`VID 0x05AC`, `PID 0x1209`)를 구성합니다. `y1_ipod_ensure_device_files()`는 iTunes/libgpod 인식을 위해 `/mnt/sdcard/iPod_Control/Device/SysInfo` 및 `SysInfoExtended`를 생성합니다.
- **`iTunesDB` 및 재생목록 읽기:** `y1_ipod_native_is_active()`는 `/mnt/sdcard/iPod_Control/iTunes/iTunesDB` 존재 여부를 확인합니다. `y1_ipod_native_load_ramcache()`는 `iTunesDB`를 Rockbox 호환 태그 테이블(`struct y1_ipod_ramcache_data`)로 파싱하며, 사용 후 `y1_ipod_native_free_ramcache()`로 해제합니다. USB 연결 해제 후에는 갱신된 DB를 다시 파싱하도록 `y1_ipod_native_invalidate()`를 호출해야 합니다.
- **앨범아트:** `y1_ipod_native_load_albumart_rgb565()`는 `ArtworkDB`에서 트랙을 조회하여 해당 `.ithmb` 슬롯을 추출하고 `*out_w` x `*out_h` 크기의 RGB565 버퍼(`*out_rgb565`)를 동적 할당합니다. 호출 측에서 `free(*out_rgb565)`로 해제해야 합니다.
- **재생 횟수, 별점 및 즐겨찾기:** `y1_ipod_native_record_play()`, `y1_ipod_native_record_rating()`, `y1_ipod_native_record_favorite()`는 변경 사항을 `iPod_Control/iTunes/Play Counts` 및 `OTGPlaylistInfo`에 기록하여 다음 USB 연결 시 iTunes와 동기화되도록 합니다.
- 바이너리 포맷 및 USB 디스크립터 세부 사항은 [IPOD_SYNC.md](IPOD_SYNC.md)를 참고하십시오.

---

## 4. 패키징, 디버깅 및 워치독

### 4.1 ConfigApp 실행(Launch) 및 디버그 정보
- **실행(Launch - 숨김 폴더 자동 탐색):** `/mnt/sdcard/`의 숨김 폴더(폴더명에 `.` 포함, 예: `/.meui/meui`, `/.custom/launch`) 내부에 ELF 실행 파일 또는 `chmod +x` 스크립트를 두면 ConfigApp **실행** 페이지에서 자동으로 감지하여 실행합니다.
- **디버그 정보:** ConfigApp의 **디버그 정보** 페이지에서 실시간 하드웨어 상태를 확인하거나 `/mnt/sdcard/debuginfoYYYYMMDD_HHMMSS.txt`를 저장할 수 있습니다.
- **Wi-Fi 및 블루투스 (`y1_wifi.c`, `y1_bt.c`):** `y1_wifi_*` 및 `y1_bt_*`는 MT6572 CONNSYS(`/dev/stpwmt`, `/dev/stpbt`, `/dev/wmtWifi`, `wlan_mt6582.ko`)를 직접 제어합니다.

### 4.2 USB 시리얼 셸 (ConfigApp 시리얼 터미널)
1. 전원 켤 때 **MENU** 버튼을 누르거나 Rockbox **메인 메뉴 -> 시스템 -> 시스템 설정**에서 **시리얼 터미널** -> **시리얼 콘솔**을 켭니다(SD 카드 미인식 시 텍스트 GUI에서 **Serial Console** 선택).
2. USB-C 케이블로 연결하고 `115200 8N1` 시리얼 터미널(Linux/macOS: `/dev/ttyACM0`, Windows: `COMx`)을 엽니다.
3. 루트 셸은 `/dev/ttyGS0` 및 `/dev/ttyGS1`에 바인딩됩니다.

### 4.3 하드웨어 안전 워치독
PID 1은 유저스페이스 애플리케이션과 독립적으로 물리 **재생/일시정지(Play/Pause)** `evdev` 키를 감시합니다. **재생/일시정지** 버튼을 10초 동안 연속으로 누르고 있으면 `sync()`를 호출한 뒤 강제 전원 종료를 수행하므로, 유저스페이스 코드가 멈추거나 교착 상태에 빠져도 배터리를 분리하지 않고 복구할 수 있습니다.

---

## 5. 최소 예제 및 Makefile

```c
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "liby1.h"

int main(void) {
    if (y1_init() != 0)
        return 1;

    y1_cpu_set_mode(Y1_CPU_MODE_PERFORMANCE);
    y1_backlight_set_percent(80);

    uint16_t fb[Y1_FB_WIDTH * Y1_FB_HEIGHT];
    memset(fb, 0, sizeof(fb));

    bool running = true;
    while (running) {
        struct y1_input_event ev;
        while (y1_input_poll(&ev, 0) > 0) {
            if (ev.value == 1 && ev.button == Y1_BTN_MENU)
                running = false;
        }
        y1_fb_present_rgb565(fb);
        usleep(16000);
    }

    y1_cpu_set_mode(Y1_CPU_MODE_DYNAMIC);
    y1_shutdown();
    return 0;
}
```

```makefile
CC       := arm-linux-gnueabihf-gcc
CFLAGS   := -O2 -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -Wall -Wextra
INCLUDES := -I../liby1/include
LIBY1_A  := ../liby1/build/liby1.a
LDFLAGS  := -lpthread -lm -lrt

TARGET   := launch
SRCS     := main.c

all: $(TARGET)

$(TARGET): $(SRCS) $(LIBY1_A)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) $(LIBY1_A) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
```
