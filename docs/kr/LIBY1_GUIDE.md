🌐 **한국어** | [English](../LIBY1_GUIDE.md)

# `liby1` — Innioasis Y1 하드웨어 추상화 레이어 및 포팅 가이드

`liby1`은 **Innioasis Y1**(MediaTek MT6572 SoC, MT6323 PMIC, MT6627 무선 칩셋)의 모든 하드웨어 드라이버, sysfs 제어 노드, 커널 ioctl 인터페이스를 감싸는 외부 종속성 제로의 독립형 C 라이브러리입니다.

개발자는 커널 레지스터 맵이나 저수준 ioctl을 역공학할 필요 없이, 검증된 공식 하드웨어 추상화 계층(HAL)을 활용하여 독립형 게임, 에뮬레이터, 커스텀 미디어 플레이어(MeUI, Reborn 등), GUI 프런트엔드 및 유틸리티를 손쉽게 개발하고 포팅할 수 있습니다.

---

## 목차
1. [타깃 플랫폼 및 하드웨어 사양](#1-타깃-플랫폼-및-하드웨어-사양)
2. [소프트웨어 실행 환경 및 부트로더 연동](#2-소프트웨어-실행-환경-및-부트로더-연동)
3. [SD 카드 디렉터리 레이아웃](#3-sd-카드-디렉터리-레이아웃)
4. [서브시스템 API 레퍼런스](#4-서브시스템-api-레퍼런스)
   - [4.1 코어, 전원, 배터리 원격 측정 (`y1_core`)](#41-코어-전원-배터리-원격-측정-y1_core)
   - [4.2 디스플레이, 프레임버퍼, 백라이트 및 절전 (`y1_display`)](#42-디스플레이-프레임버퍼-백라이트-및-절전-y1_display)
   - [4.3 CPU 주파수 거버너 및 듀얼코어 핫플러그 (`y1_cpu`)](#43-cpu-주파수-거버너-및-듀얼코어-핫플러그-y1_cpu)
   - [4.4 입력: 클릭 휠, 버튼 및 3.5mm 단자 (`y1_input`)](#44-입력-클릭-휠-버튼-및-35mm-단자-y1_input)
   - [4.5 오디오: EAC DMA 및 MT6323 DAC/앰프 (`y1_audio`)](#45-오디오-eac-dma-및-mt6323-dac앰프-y1_audio)
   - [4.6 FM 라디오 튜너 (`y1_tuner`)](#46-fm-라디오-튜너-y1_tuner)
   - [4.7 USB 대용량 저장소 가젯 (`y1_usb`)](#47-usb-대용량-저장소-가젯-y1_usb)
5. [포팅 가이드: Y1에서 나만의 경험 제작하기](#5-포팅-가이드-y1에서-나만의-경험-제작하기)
   - [5.1 표준 Y1 메인 루프 아키텍처](#51-표준-y1-메인-루프-아키텍처)
   - [5.2 프레임버퍼 / SDL / Raylib / LVGL 애플리케이션 포팅](#52-프레임버퍼--sdl--raylib--lvgl-애플리케이션-포팅)
   - [5.3 MeUI, Reborn, 커스텀 소프트웨어 패키징](#53-meui-reborn-커스텀-소프트웨어-패키징)
6. [대화형 USB 시리얼 터미널 디버깅 (선택지 8)](#6-대화형-usb-시리얼-터미널-디버깅-선택지-8)
7. [하드웨어 안전 워치독 (10초 강제 전원 끄기)](#7-하드웨어-안전-워치독-10초-강제-전원-끄기)
8. [완전한 동작 예제: 대화형 게임 데모](#8-완전한-동작-예제-대화형-게임-데모)
9. [빌드 환경 구성 및 Makefile 템플릿](#9-빌드-환경-구성-및-makefile-템플릿)

---

## 1. 타깃 플랫폼 및 하드웨어 사양

| 하드웨어 항목 | Innioasis Y1 사양 상세 |
| :--- | :--- |
| **SoC** | MediaTek MT6572 (2x ARM Cortex-A7 @ 598 MHz ~ 1.30 GHz) |
| **명령어 세트** | ARMv7-A NEON SIMD 및 VFPv4 (타깃: `arm-linux-gnueabihf`) |
| **메모리 (RAM)**| 512 MB LPDDR2 |
| **디스플레이** | 2.8인치 IPS LCD, **480x360 기본 해상도** (RGB565 / XRGB8888, `/dev/fb0`) |
| **오디오 DAC/앰프**| MediaTek MT6323 PMIC 스테레오 DAC & 아날로그 PGA 헤드폰 앰프 (8 kHz ~ 48 kHz, 16-bit 스테레오) |
| **입력 장치** | 정전식 터치 클릭 휠 (시계/반시계 방향 회전 틱), 5개 휠 버튼 (선택, 메뉴, 재생/일시정지, 이전, 다음) |
| **헤드폰 단자**| 3.5mm TRRS 단자 (플러그 삽입 감지, 마이크 바이어스, 인라인 MFB 리모컨 감지 지원) |
| **연결성** | USB 2.0 고속 OTG/클라이언트 (대용량 저장소, USB 시리얼 터미널 ACM), 블루투스 4.0, FM 라디오 |
| **저장소** | 외장 MicroSD 슬롯 (SDSC, SDHC, 최대 2TB SDXC 지원, FAT32/exFAT/ext4) |

---

## 2. 소프트웨어 실행 환경 및 부트로더 연동

Y1의 부트로더 initramfs(`init.c`)는 외장 MicroSD 카드에 저장된 다양한 소프트웨어를 동적으로 감지하여 바로 실행할 수 있도록 지원합니다.

### 동적 부트로더 캐러셀 메뉴 (부팅 시 `MENU` 유지)
기기 전원을 켤 때 **MENU** 버튼을 길게 누르고 있으면 시각적 캐러셀 메뉴가 나타납니다. SD 카드에 실제로 존재하는 프로그램만 자동으로 목록에 표시됩니다:

1. **Pick 1: Rockbox** — 기본 오디오 플레이어 (`/.rockbox/rockbox` 또는 `/mnt/sdcard/.rockbox/rockbox`).
2. **Pick 2: MeUI** — `/mnt/sdcard/.meui/meui` 또는 `/.meui/meui`가 존재할 때만 표시.
3. **Pick 3: Reborn** — `/mnt/sdcard/.reborn/reborn` 또는 `/.reborn/reborn`이 존재할 때만 표시.
4. **Pick 4: User defined software** — `/mnt/sdcard/.custom/launch` 또는 `/.custom/launch`가 존재할 때만 표시.
5. **Pick 5: Android** — 기기 내장 순정 안드로이드 파티션으로 즉시 재부팅.
6. **Pick 6: Shut down** — 즉시 안전한 파일시스템 동기화 및 하드웨어 전원 끄기.
7. **Pick 7: USB Mass Storage** — PC와 음악 및 파일 전송 모드 연결.
8. **Pick 8: Linux terminal (USB)** — USB 케이블을 통해 `/dev/ttyGS0` 시리얼 루트 셸 실행.

### 런타임 환경
애플리케이션이 실행될 때의 기본 환경 변수:
- `PATH=/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sdcard/.system/bin:/mnt/sdcard/.rockbox/bin:/mnt/sdcard/.meui/bin:/mnt/sdcard/.reborn/bin:/mnt/sdcard/.custom/bin`
- `LD_LIBRARY_PATH=/mnt/sdcard/.meui/lib:/mnt/sdcard/.reborn/lib:/mnt/sdcard/.custom/lib:/mnt/sdcard/.system/lib:/mnt/sdcard/.rockbox/lib:/lib:/usr/lib`
- `HOME=/mnt/sdcard`
- `TERM=linux`

---

## 3. SD 카드 디렉터리 레이아웃

별도의 플래싱이나 루팅 없이 MicroSD 카드에 파일을 복사하기만 하면 됩니다:

```text
/mnt/sdcard/ (MicroSD 루트)
|-- .system/                      # 공용 시스템 폴더 (부팅 시 /.system에 심볼릭 링크)
|   |-- include/liby1.h           # 공개 C 헤더 파일
|   |-- lib/liby1.a               # 오프라인 링크용 정적 라이브러리
|   |-- lib/liby1.so              # 공유 라이브러리
|   |-- lib/liby1video.so         # NEON H.264/AAC 하드웨어 가속 비디오 디코더
|   |-- y1_debug.txt              # 실시간 하드웨어 진단 로그
|   `-- app                       # 단일 독립형 자동 실행 프로그램
|-- .custom/                      # 선택지 4: 사용자 정의 소프트웨어
|   |-- launch                    # 실행 파일 (ELF 바이너리 또는 셸 스크립트)
|   `-- lib/                      # 커스텀 공유 라이브러리 (.so)
|-- .meui/                        # 선택지 2: MeUI 프런트엔드
|   `-- meui                      # 실행 파일
|-- .reborn/                      # 선택지 3: Reborn 음악 플레이어
|   `-- reborn                    # 실행 파일
`-- .rockbox/                     # 선택지 1: Rockbox 플레이어
    `-- rockbox                   # Rockbox 바이너리
```

---

## 4. 서브시스템 API 레퍼런스

모든 선언은 [`hosted_linux_port/liby1/include/liby1.h`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/hosted_linux_port/liby1/include/liby1.h)에 정의되어 있습니다.

### 4.1 코어, 전원, 배터리 원격 측정 (`y1_core`)
```c
#include "liby1.h"

int  y1_init(void);     // 디스플레이, 백라이트, 입력, 오디오 하드웨어 초기화
void y1_shutdown(void); // 안전한 하드웨어 종료

void y1_log(const char *msg);
void y1_logf(const char *fmt, ...);

int  y1_battery_percent(void);    // 배터리 잔량: 0 ~ 100 %
int  y1_battery_voltage_mv(void); // 실시간 전압 (mV 단위, 예: 3850 mV)
int  y1_battery_current_ma(void); // 실시간 충/방전 전류 (mA)
bool y1_battery_is_charging(void);
bool y1_power_usb_online(void);   // USB 또는 AC 전원 연결 여부

void y1_sys_poweroff(void); // 파일시스템 sync 후 하드웨어 전원 차단
void y1_sys_reboot(void);   // SoC 재부팅
```

### 4.2 디스플레이, 프레임버퍼, 백라이트 및 절전 (`y1_display`)
```c
int      y1_fb_init(void);
void     y1_fb_close(void);
int      y1_fb_width(void);       // 480
int      y1_fb_height(void);      // 360
uint8_t *y1_fb_mmap_ptr(void);    // 32비트 프레임버퍼 메모리 직접 포인터

void y1_fb_present_rgb565(const uint16_t *pixels_480x360);
void y1_fb_present_xrgb8888(const uint32_t *pixels_480x360);
void y1_fb_flip(void);            // 하드웨어 오버레이 화면 갱신 (FBIOPAN_DISPLAY)

// 레트로 320x240 게임을 480x360으로 부드럽게 2x2->3x3 면적 평균 하드웨어 업스케일링
void y1_fb_blit_rgb565_classic_scaled(const uint16_t *src_base, int src_stride_px,
                                      int x, int y, int width, int height,
                                      y1_pixel_blend_fn blend_cb, void *blend_user);

void y1_backlight_set_percent(int pct_0_to_100);
void y1_display_enter_early_suspend(void); // 화면, 백라이트, 터치 스캔, GPU 클럭 차단
void y1_display_exit_early_suspend(void);  // 화면 복구
```

### 4.3 CPU 주파수 거버너 및 듀얼코어 핫플러그 (`y1_cpu`)
```c
y1_cpu_set_mode(Y1_CPU_MODE_POWERSAVE);   // 598 MHz (최저 VCORE 1.05V, 배터리 절약)
y1_cpu_set_mode(Y1_CPU_MODE_DYNAMIC);     // Ondemand 거버너 (598 MHz ~ 1.00 GHz)
y1_cpu_set_mode(Y1_CPU_MODE_PERFORMANCE); // 고정 1.20 GHz (에뮬레이터 및 게임 권장)
y1_cpu_set_mode(Y1_CPU_MODE_TURBO);       // 고정 1.30 GHz

y1_cpu_set_dual_core(false);              // CPU1 코어 끄기 (소비전력 절감)
y1_cpu_set_dual_core(true);               // 듀얼코어 활성화
int freq = y1_cpu_get_freq_mhz();         // 현재 클럭 조회 (MHz)
```

### 4.4 입력: 클릭 휠, 버튼 및 3.5mm 단자 (`y1_input`)
```c
int y1_input_init(void);

struct y1_input_event ev;
// timeout_ms: 0 = 즉시 반환(non-blocking), -1 = 무한 대기
if (y1_input_poll(&ev, 16) > 0) {
    if (ev.value == 1) { // 버튼 눌림
        switch (ev.button) {
            case Y1_BTN_SELECT:      /* 가운데 선택 버튼 */ break;
            case Y1_BTN_MENU:        /* 상단 메뉴 버튼 */ break;
            case Y1_BTN_PLAY:        /* 하단 재생/일시정지 버튼 */ break;
            case Y1_BTN_LEFT:        /* 좌측 이전 곡 버튼 */ break;
            case Y1_BTN_RIGHT:       /* 우측 다음 곡 버튼 */ break;
            case Y1_BTN_SCROLL_FWD:  /* 휠 시계 방향 회전 */ break;
            case Y1_BTN_SCROLL_BACK: /* 휠 반시계 방향 회전 */ break;
            case Y1_BTN_HEADSET_MFB: /* 이어폰 인라인 리모컨 버튼 */ break;
        }
    }
}

bool inserted = y1_jack_is_inserted(); // 이어폰 연결 여부
bool has_mic  = y1_jack_has_mic();     // 4극 마이크 지원 이어폰 여부
```

### 4.5 오디오: EAC DMA 및 MT6323 DAC/앰프 (`y1_audio`)
```c
if (y1_audio_open(44100) == 0) {
    // 아날로그 PGA 볼륨 (0 ~ 7: 6 = 0 dB 유니티 게인, 7 = +2 dB)
    y1_audio_set_hp_analog_gain(6, 6);
    y1_audio_wake_lock(true); // 재생 중 절전 모드 진입 방지

    int16_t pcm_buffer[1024 * 2]; // 16-bit 인터리브 스테레오 PCM
    y1_audio_write_pcm(pcm_buffer, sizeof(pcm_buffer));

    y1_audio_wake_lock(false);
    y1_audio_close();
}
```

---

## 5. 포팅 가이드: Y1에서 나만의 경험 제작하기

### 5.1 표준 Y1 메인 루프 아키텍처
```c
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include "liby1.h"

int main(void) {
    if (y1_init() != 0) {
        fprintf(stderr, "liby1 초기화 실패\n");
        return 1;
    }

    y1_cpu_set_mode(Y1_CPU_MODE_PERFORMANCE);
    y1_backlight_set_percent(80);

    uint16_t frame[480 * 360];
    bool running = true;

    while (running) {
        struct y1_input_event ev;
        while (y1_input_poll(&ev, 0) > 0) {
            if (ev.value == 1 && ev.button == Y1_BTN_MENU)
                running = false; // 메뉴 버튼으로 종료
        }

        // 여기에 렌더링 로직 작성 (frame 버퍼에 픽셀 쓰기)

        y1_fb_present_rgb565(frame); // 화면에 전송
        usleep(16000); // 약 60 FPS 제어
    }

    y1_cpu_set_mode(Y1_CPU_MODE_DYNAMIC);
    y1_shutdown();
    return 0;
}
```

### 5.2 프레임버퍼 / SDL / Raylib / LVGL 애플리케이션 포팅

1. **캔버스 할당 및 전송:**
   렌더링 버퍼(`uint16_t canvas[480 * 360]`)를 선언하고, 프레임 완성 시 `y1_fb_present_rgb565(canvas)`를 호출합니다.
2. **레트로 게임 320x240 스케일링:**
   Game Boy, NES, DOOM 등 320x240 기반 엔진은 직접 업스케일링하지 말고 `y1_fb_blit_rgb565_classic_scaled(source_320x240, 320, 0, 0, 320, 240, NULL, NULL)`을 호출하면 하드웨어 가속으로 480x360 화면 전체에 매끄럽게 출력됩니다.

### 5.3 MeUI, Reborn, 커스텀 소프트웨어 패키징

- **선택지 4 (사용자 정의 소프트웨어):** 실행 파일을 `/mnt/sdcard/.custom/launch` 경로에 저장하고 실행 권한(`chmod +x`)을 부여합니다.
- **선택지 2 (MeUI) / 선택지 3 (Reborn):** 바이너리를 각각 `/mnt/sdcard/.meui/meui` 또는 `/mnt/sdcard/.reborn/reborn`에 저장합니다.

---

## 6. 대화형 USB 시리얼 터미널 디버깅 (선택지 8)

개발 중 실시간 로그 확인, 프로세스 제어, 터미널 명령 실행이 가능합니다.

1. 부팅 시 **MENU** 버튼을 눌러 캐러셀 메뉴로 진입합니다.
2. **선택지 8: Linux terminal (USB)**를 선택합니다.
3. PC와 USB-C 케이블로 연결합니다.
4. PC 시리얼 터미널 프로그램(`115200 8N1`, Windows COM 포트 또는 Linux `/dev/ttyACM0`)을 실행합니다:
   ```bash
   py -m serial.tools.miniterm COM5 115200
   ```
5. 기기 내부 루트 셸 프롬프트가 즉시 열립니다:
   ```text
   Nugget for Y1 terminal
          Welcome! Root shell active on /dev/ttyGS0.

   y1-linux:# ps
   y1-linux:# top
   y1-linux:# cd /mnt/sdcard/.custom && ./launch
   ```

---

## 7. 하드웨어 안전 워치독 (10초 강제 전원 끄기)

프로그램이 세그멘테이션 오류, 무한 루프, 데드락 등으로 완전히 멈추더라도 배터리를 분리할 필요가 없습니다:
- **PID 1에서 독립 백그라운드 워치독이 항상 동작합니다.**
- **재생/일시정지(Play/Pause) 버튼을 10초간 연속으로 누르면 어떤 상황에서든 안전하게 파일시스템을 동기화하고 강제로 전원을 차단합니다.**

---

## 8. 완전한 동작 예제: 대화형 게임 데모

```c
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "liby1.h"

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color) {
    for (int r = y; r < y + h; r++) {
        if (r < 0 || r >= Y1_FB_HEIGHT) continue;
        for (int c = x; c < x + w; c++) {
            if (c < 0 || c >= Y1_FB_WIDTH) continue;
            fb[r * Y1_FB_WIDTH + c] = color;
        }
    }
}

int main(void) {
    if (y1_init() != 0) return 1;

    y1_cpu_set_mode(Y1_CPU_MODE_PERFORMANCE);
    y1_backlight_set_percent(85);

    uint16_t *canvas = malloc(Y1_FB_WIDTH * Y1_FB_HEIGHT * sizeof(uint16_t));
    int paddle_x = 200, ball_x = 240, ball_y = 180, ball_dx = 3, ball_dy = 3;

    bool running = true;
    while (running) {
        struct y1_input_event ev;
        while (y1_input_poll(&ev, 0) > 0) {
            if (ev.value == 1) {
                if (ev.button == Y1_BTN_MENU) running = false;
                else if (ev.button == Y1_BTN_LEFT || ev.button == Y1_BTN_SCROLL_BACK) paddle_x -= 20;
                else if (ev.button == Y1_BTN_RIGHT || ev.button == Y1_BTN_SCROLL_FWD) paddle_x += 20;
            }
        }

        if (paddle_x < 10) paddle_x = 10;
        if (paddle_x > Y1_FB_WIDTH - 90) paddle_x = Y1_FB_WIDTH - 90;

        ball_x += ball_dx; ball_y += ball_dy;
        if (ball_x <= 5 || ball_x >= Y1_FB_WIDTH - 15) ball_dx = -ball_dx;
        if (ball_y <= 5) ball_dy = -ball_dy;
        if (ball_y >= 320 && ball_y <= 335 && ball_x >= paddle_x && ball_x <= paddle_x + 80) ball_dy = -ball_dy;
        if (ball_y > Y1_FB_HEIGHT) { ball_x = 240; ball_y = 100; ball_dy = 3; }

        memset(canvas, 0, Y1_FB_WIDTH * Y1_FB_HEIGHT * sizeof(uint16_t));
        fill_rect(canvas, 0, 0, Y1_FB_WIDTH, 24, rgb565(20, 20, 40));
        fill_rect(canvas, 420, 6, (y1_battery_percent() * 48) / 100, 12, rgb565(0, 220, 100));
        fill_rect(canvas, paddle_x, 330, 80, 12, rgb565(255, 200, 50));
        fill_rect(canvas, ball_x, ball_y, 10, 10, rgb565(255, 80, 80));

        y1_fb_present_rgb565(canvas);
        usleep(16666);
    }

    free(canvas);
    y1_shutdown();
    return 0;
}
```

---

## 9. 빌드 환경 구성 및 Makefile 템플릿

```makefile
CC            := arm-linux-gnueabihf-gcc
CFLAGS        := -O2 -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -Wall -Wextra
INCLUDES      := -I../hosted_linux_port/liby1/include
LIBY1_A       := ../hosted_linux_port/liby1/build/liby1.a
LDFLAGS       := -lpthread -lm -lrt

TARGET        := launch
SRCS          := main.c

all: $(TARGET)

$(TARGET): $(SRCS) $(LIBY1_A)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) $(LIBY1_A) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
```
빌드된 `launch` 실행 파일을 `/mnt/sdcard/.custom/launch`에 넣으면 부트로더 메뉴에서 바로 실행할 수 있습니다!
