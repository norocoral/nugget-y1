🌐 **한국어** | [English](README.md)

# `liby1` — Innioasis Y1 하드웨어 추상화 레이어

`liby1`은 **Innioasis Y1** 디지털 오디오 플레이어의 하드웨어 드라이버, MediaTek MT6572 / MT6323 PMIC 동작 및 커널 인터페이스를 감싸는 외부 종속성 제로의 가벼운 C 라이브러리입니다.

C/C++ 개발자는 복잡한 커널 ioctl, PMIC 레지스터, sysfs 경로를 분석할 필요 없이 Y1을 위한 독립형 소프트웨어, 게임, 에뮬레이터, 커스텀 미디어 플레이어(MeUI 등)를 자유롭게 개발할 수 있습니다. Rockbox 역시 내부적으로 `liby1`을 기반으로 구동됩니다.

> [!TIP]
> 전체 API 레퍼런스, 게임 루프 튜토리얼, 아키텍처 및 포팅 가이드는 **[`liby1` 개발자 매뉴얼 및 포팅 가이드](../docs/kr/LIBY1_GUIDE.md)** ([English](../docs/LIBY1_GUIDE.md))에서 확인하실 수 있습니다.

---

## SD 카드 멀티 부트 및 `.system` 디렉터리 구성

Y1 부트로더는 외장 MicroSD 카드에 저장된 소프트웨어를 동적으로 자동 감지합니다:

| 경로 | 용도 |
| :--- | :--- |
| `/mnt/sdcard/.meui/meui` | **부트로더 캐러셀 선택지 2:** MeUI 프런트엔드 (자동 감지) |
| `/mnt/sdcard/.custom/launch` | **부트로더 캐러셀 선택지 3:** 사용자 정의 소프트웨어 (자동 감지) |
| `/mnt/sdcard/.system/app` | 단일 독립형 자동 실행 프로그램 (메뉴 없이 부팅 시 바로 실행) |
| `/mnt/sdcard/.system/lib/liby1.a` | 오프라인 정적 링크용 하드웨어 라이브러리 |
| `/mnt/sdcard/.system/lib/liby1.so` | 공유 동적 라이브러리 |
| `/mnt/sdcard/.system/include/liby1.h`| `liby1` 공개 C 헤더 파일 |
| `/mnt/sdcard/.system/lib/liby1video.so`| NEON 가속 H.264 / AAC 비디오 디코더 |
| `/mnt/sdcard/.system/y1_debug.txt` | 실시간 시스템 및 하드웨어 진단 로그 |

---

## 제공 서브시스템

1. **코어, 전원, 배터리 및 로깅 (`y1_core.c`)**
   - `y1_init()`, `y1_shutdown()`, `y1_battery_percent()`, `y1_battery_voltage_mv()`, `y1_battery_is_charging()`
2. **디스플레이 및 백라이트 (`y1_display.c`)**
   - `y1_fb_init()`, `y1_fb_present_rgb565()`, `y1_fb_present_xrgb8888()`, `y1_fb_flip()`
   - `y1_fb_blit_rgb565_classic_scaled()` (레트로 320x240 게임을 480x360으로 부드럽게 2x2->3x3 면적 평균 하드웨어 업스케일)
   - `y1_backlight_set_percent()`, `y1_display_enter_early_suspend()`, `y1_display_exit_early_suspend()`
3. **CPU 주파수 및 듀얼코어 핫플러그 (`y1_cpu.c`)**
   - `y1_cpu_set_mode()`, `y1_cpu_set_dual_core()`, `y1_cpu_get_freq_mhz()`
4. **입력: 클릭 휠 및 3.5mm 단자 (`y1_input.c`)**
   - `y1_input_init()`, `y1_input_poll()`, `y1_jack_is_inserted()`, `y1_jack_has_mic()`
5. **오디오 DAC 및 헤드폰 앰프 (`y1_audio.c`)**
   - `y1_audio_open()`, `y1_audio_write_pcm()`, `y1_audio_set_hp_analog_gain()`, `y1_audio_wake_lock()`
6. **FM 라디오 튜너 (`y1_tuner.c`)**
   - `y1_fm_power()`, `y1_fm_tune()`, `y1_fm_get_rssi()`
7. **USB 대용량 저장소 가젯 (`y1_usb.c`)**
   - `y1_usb_attach_mass_storage()`, `y1_usb_detach_mass_storage()`, `y1_usb_check_host_ejected()`
   - `y1_usb_tune_performance()` (`mmcblk1` 블록 큐 튜닝: `noop`/`deadline`, 2 MiB `read_ahead_kb`, `nr_requests=512`, `max_sectors_kb` 및 USB 연결 중 1.3 GHz 듀얼코어 고정)
   - `y1_usb_set_ipod_mode()`, `y1_usb_get_ipod_mode()` (Apple iPod 5.5G Enhanced VID/PID, `ipod_vpd.ko` EVPD XML 및 단일 LUN 스푸핑)
8. **iPod 5.5G USB 모드 및 네이티브 iTunes 엔진 (`y1_ipod_sync.c`)**
   - `y1_cpu_boost_for_conversion()` (고부하 작업 중 1300 MHz 듀얼코어 퍼포먼스 강제 고정)
   - `y1_ipod_ensure_device_files()` (`/iPod_Control/Device/SysInfo` 및 `SysInfoExtended` XML 생성)
   - `y1_ipod_native_is_active()`, `y1_ipod_native_invalidate()`, `y1_ipod_native_load_ramcache()`, `y1_ipod_native_free_ramcache()` (`.tcd` 변환 없이 `iTunesDB`를 Rockbox TagCache RAM으로 직접 로드)
   - `y1_ipod_native_has_albumart()`, `y1_ipod_native_load_albumart_rgb565()` (`ArtworkDB` + `F*.ithmb` RGB565 실시간 디코딩)
   - `y1_ipod_native_playlist_count()`, `y1_ipod_native_playlist_name()`, `y1_ipod_native_playlist_track_count()`, `y1_ipod_native_playlist_track_path()` (`iTunesDB` 재생목록 및 `SLst` 스마트 재생목록 메모리 직접 조회)
   - `y1_ipod_native_record_play()`, `y1_ipod_native_record_rating()`, `y1_ipod_native_record_favorite()` (실시간 `/iPod_Control/iTunes/Play Counts` `mhdp` 및 `OTGPlaylistInfo` `mhpo` 기록)
   - 독립 CLI 도구: `/bin/ipod_sync init`

---

## 개발자 워크플로

### 1. 독립형 프로그램 빌드
```bash
# liby1 빌드
make -C hosted_linux_port/liby1 CROSS_COMPILE=arm-linux-gnueabihf-

# 내 애플리케이션 정적 컴파일
arm-linux-gnueabihf-gcc -O2 -Ihosted_linux_port/liby1/include \
    my_app.c hosted_linux_port/liby1/build/liby1.a -o launch -lpthread -lm -lrt
```

### 2. USB 대화형 시리얼 터미널 디버깅 (선택지 7)
부팅 시 **MENU**를 눌러 **선택지 7: Linux terminal (USB)**을 선택하고 PC에서 터미널(`115200 8N1`, `COMx` 또는 `/dev/ttyACM0`)을 열면 `top`, `ps`, `dmesg` 등 실시간 디버깅 셸을 바로 사용할 수 있습니다.

### 3. 안전 워치독
개발 중 프로그램이 데드락이나 무한 루프에 빠지더라도 **Play/Pause 버튼을 10초간 길게 누르면** 하드웨어 강제 전원 차단이 이루어집니다.

---

## 관련 문서
- [iPod 5.5G USB Mode & Sync Engine Guide (English)](../docs/IPOD_SYNC.md)
- [iPod 5.5G USB 모드 및 동기화 엔진 가이드 (한국어)](../docs/kr/IPOD_SYNC.md)
- [liby1 Complete Developer Manual & Porting Guide (English)](../docs/LIBY1_GUIDE.md)
- [liby1 개발자 매뉴얼 및 포팅 가이드 (한국어)](../docs/kr/LIBY1_GUIDE.md)
- [모듈식 빌드 시스템 가이드 (한국어)](../docs/kr/BUILD_GUIDE.md)
