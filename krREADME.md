<img width="1440" height="1080" alt="herokr" src="https://github.com/user-attachments/assets/e2ccc92e-e80b-4137-9537-2affff1bcf59" />

🌐 **한국어** | [English](README.md)

# Nugget for Innioasis Y1

[![Platform](https://img.shields.io/badge/Platform-MediaTek%20MT6572-orange.svg)](#하드웨어-사양)

Nugget for Y1은 Innioasis Y1 디지털 오디오 플레이어를 위한 오픈 소스 커스텀 펌웨어입니다. 안드로이드를 최소 구성 다운스트림 리눅스 커널(3.4.67)로 완전히 대체하며, 개발자가 네이티브 환경에서 직접 애플리케이션을 구현할 수 있도록 개발자 라이브러리를 제공합니다.

---

## 기술 비교

| 항목 | 순정 안드로이드 | 안드로이드 위 Rockbox | Nugget for Y1 (호스티드 리눅스) |
| :--- | :--- | :--- | :--- |
| OS / 런타임 | 안드로이드 (MediaTek MT6572 유저랜드, Zygote/Dalvik) | 안드로이드 유저랜드 내부에서 실행되는 Rockbox APK | 최소 구성 리눅스 initramfs (커널 3.4.67)에서 Hosted Rockbox 직접 부팅 (`PID 1 /init`) |
| 오디오 출력 경로 | 안드로이드 AudioFlinger -> 안드로이드 오디오 HAL -> `/dev/eac` | 안드로이드 AudioFlinger (`AudioTrack`/OpenSL) -> 안드로이드 오디오 HAL -> `/dev/eac` | `pcm-eac.c`에서 `/dev/eac`로 직접 출력 (AudioFlinger 미경유) |
| 샘플 레이트 처리 | AudioFlinger에서 리샘플링/믹싱 | AudioFlinger에서 리샘플링/믹싱 | 44.1 kHz 및 48 kHz 하드웨어 클록 직접 전환 |
| 볼륨 / 게인 제어 | 안드로이드 오디오 스트림 단계 + MT6323 아날로그 PGA | Rockbox 소프트웨어 볼륨 + 안드로이드 미디어 스트림 | Rockbox 로그 스케일 볼륨을 PCM 감쇠 + MT6323 아날로그 PGA(`0x70a`)에 매핑 |
| 블루투스 오디오 | 안드로이드 블루투스 스택 (Bluedroid) | 안드로이드 블루투스 스택 (Bluedroid) | `liby1` 직접 MT6572 CONNSYS (`/dev/stpwmt`, `/dev/stpbt`) + 커스텀 A2DP (SBC, SBC-XQ, AAC) 및 수신기 모드 |
| 전원 / 절전 | 안드로이드 `earlysuspend` + `wakelock` (`mem` 절전) | 안드로이드 `earlysuspend` + `wakelock` (`mem` 절전) | 직접 `wakelock` / `earlysuspend` + `/sys/power/state` (`mem` 절전) + CPU 핫플러그/거버너 제어 |
| 디스플레이 해상도 | 480x360 | APK 빌드에 따라 다름 (일반적으로 480x360) | 480x360 네이티브 및 런타임 320x240 스케일링 모드 |
| 키 잠금 (HOLD) | 없음 | Rockbox 키 잠금 | Rockbox 키 잠금 (MENU 더블 클릭 토글, 홀드 라이트, 잠금 백라이트 규칙, 예외 키 설정) |
| USB 모드 | MTP / 안드로이드 USB 대용량 저장소 | MTP / 안드로이드 USB 대용량 저장소 (OS 경유) | USB 대용량 저장소 및 선택적 Apple iPod 5.5세대 에뮬레이션 (`ipod_vpd.ko`, 기본 비활성화) |
| iTunes 연동 | 없음 | 없음 | 선택적 `iTunesDB` / `ArtworkDB` / `.ithmb` TagCache 및 재생목록 파싱, `Play Counts` / `OTGPlaylistInfo` 기록 |
| 듀얼 부팅 | 순정 OS 단일 부팅 | 안드로이드 앱 | 부팅 시 **ConfigApp** (`--bootmenu`) 실행 (숨김 폴더 앱 자동 탐색, 안드로이드 전환, USB, 시리얼 터미널, Wi-Fi/BT, SD 미인식 시 텍스트 GUI 지원) |

---

## 기술 문서

서브시스템 사양, 아키텍처 문서 및 리버스 엔지니어링 분석 내용은 [`docs/kr/`](docs/kr/)에 있습니다:
- [iPod 5.5G USB 모드 및 iTunes 연동](docs/kr/IPOD_SYNC.md): `ipod_vpd.ko` EVPD 스푸핑, `iTunesDB` TagCache 로드, `.ithmb` 앨범아트 디코딩, `Play Counts`/`OTGPlaylistInfo` 기록 및 USB 가젯 구성.
- [기술 분석 결과](docs/kr/TECHNICAL_FINDINGS.md): 하드웨어 인터페이스, `/dev/eac` 오디오 드라이버, PMIC 레지스터, 전력 튜닝, 부팅 측정 결과 및 입력 이벤트 동작.
- [시스템 아키텍처](docs/kr/ARCHITECTURE.md): 아키텍처 블록 다이어그램, 부팅 시퀀스, 스토리지 레이아웃 및 HAL 매핑.
- [오디오 서브시스템](docs/kr/AUDIO_SUBSYSTEM.md): MediaTek EAC 드라이버, 팝 노이즈 완화, 클록 전환 및 게인 스테이징.
- [전원 관리](docs/kr/POWER_MANAGEMENT.md): 절전/wakelock 처리, CPU 주파수 거버너, 코어 핫플러그 및 Always-On Display (AOD).

---

## 설치 방법

### 권장: Innioasis Updater CE
https://innioasis.app/firmware_ko.html?brand=Innioasis&model=Y1&software=nugget-rock8ox-240p-360p-y1&type=A

### 수동 설치 방법
1. `boot_rockbox.img` 및 `rockbox_sdcard.zip`을 준비합니다.
2. 다음 도구를 사용하여 `boot_rockbox.img`를 `boot` 파티션에 플래시합니다:
   - SP Flash Tool: MT6572 scatter 로드, `BOOTIMG` 체크, `boot_rockbox.img` 선택 후 Download 클릭, 볼륨 다운 버튼을 누른 채로 Y1 연결.
   - Fastboot / python-mtk:
     ```bash
     python mtk.py w boot boot_rockbox.img
     ```
3. MicroSD 카드를 FAT32 또는 exFAT 형식으로 포맷합니다.
4. `.rockbox` 디렉터리가 최상위 루트에 위치하도록 `rockbox_sdcard.zip` 압축을 카드 루트에 직접 풉니다.
5. MicroSD 카드를 Y1에 삽입하고 전원을 켭니다.

---

## 소스 코드 빌드

### 사전 필수 패키지 (Ubuntu / Debian / WSL)
```bash
sudo apt update
sudo apt install -y build-essential gcc-arm-linux-gnueabihf python3 python3-pip zip curl
```

### 필수 외부 파일
1. **순정 `boot.img` (커널 및 안드로이드 듀얼 부팅 램디스크 추출용):**
   Y1 Type A 순정 ROM([rom.zip](https://github.com/y1-community/y1-ata-rom/releases/download/0.1/rom.zip))을 다운로드하여 `boot.img`를 추출한 뒤 `stock_rom/boot.img` 경로에 배치합니다(또는 `STOCK_BOOT=/path/to/boot.img` 지정).
2. **Rockbox 소스 (Rockbox 포함 빌드 시):**
   이 저장소와 나란히 `https://github.com/norocoral/rockbox`를 클론(`../rockbox`)하거나 `ROCKBOX_SRC=/path/to/rockbox`를 지정합니다. Rockbox 없이 ROM/SDK만 빌드하려면 `./build.sh --no-rockbox`를 실행합니다.

### 빌드 명령
```bash
./build.sh
```

빌드 스크립트 실행 동작:
1. `!stock_rom/boot.img`에서 순정 커널을 추출합니다(없는 경우).
2. ARM 하드 플로트 GCC를 사용하여 커스텀 PID 1 C `/init`, `unmute_eac`, `liby1`, `configapp`을 컴파일합니다.
3. `splash/bootorg.png`를 Wanted Sans 폰트 및 배경 대비 자동 흑/백 텍스트가 적용된 32비트 원시 프레임버퍼 파일로 변환합니다.
4. Innioasis Y1용 호스티드 록박스를 컴파일합니다(`build_y1/`).
5. `.rockbox` 및 `.system` SD 카드 리소스(`configapp`, `bt_blobs/`, `make_dist.sh`/scatter용 `logo.bin` 포함)를 `rockbox_sdcard.zip`으로 패키징합니다.
6. 최소 구성의 initramfs 램디스크를 빌드하고 MediaTek 헤더가 포함된 플래시용 `boot.img` / `boot_nugget.img`를 생성합니다.

*참고: 오픈 소스 라이선스 규정을 준수하기 위해 MediaTek 독점 바이너리(`bt_blobs/`, `stock_unpacked/`, `logo.bin`)는 Git 추적에서 제외되어 있습니다.*

---

### 듀얼 부팅 서브시스템 및 ConfigApp 부팅 메뉴

내부 eMMC의 안드로이드 파티션(`SYSTEM`, `DATA`, `CACHE`)을 변경하지 않고 유지하므로 안드로이드와 듀얼 부팅이 가능합니다:

- **ConfigApp (`--bootmenu` 및 Rockbox `메인 메뉴 -> 시스템 -> 시스템 설정`):**
  - 기존 Bootpick 메뉴를 대체하는 라이트 테마 네이티브 C UI(`/.system/configapp/configapp`, 한국어/영어 전환, 12시간제 시계)입니다.
  - **지원 페이지:**
    - **실행(Launch)** — SD 카드의 숨김 폴더(폴더명에 `.` 포함 및 내부에 실행 파일이 있는 앱) 자동 탐색 및 순정 안드로이드 전환.
    - **시스템 상태**, **화면 및 진동**, **CPU 및 전력**, **날짜 및 시간**(12시간제 시계 및 RTC).
    - **블루투스** (`y1_bt_*`) 및 **Wi-Fi** (`y1_wifi_*`, `wlan_mt6582.ko` / `mtk_wmt_wifi.ko` / `/dev/wmtWifi` 기반).
    - **USB 및 저장소** (USB 대용량 저장소, iPod 5.5세대 모드, 성능 튜닝).
    - **시리얼 터미널** (`/dev/ttyGS0` 및 `/dev/ttyGS1` 듀얼 USB ACM 루트 셸).
    - **디버그 정보** (ConfigApp UI 내 실시간 하드웨어 정보 표시 및 `debuginfo*.txt` 저장).
    - **시스템 및 전원** (언어 전환, 디버그 로그) 및 **종료 / 전원 끄기** (앱 종료, 재부팅, 전원 끄기).
  - **SD 카드 미인식 폴백:** 부팅 시 MicroSD 카드가 없으면 PID 1 `/init`이 3가지 항목(**USB**, **Serial Console**, **Shut Down**)의 기본 텍스트 GUI를 표시합니다.
  - **하드웨어 강제 진입:** 부팅 중 **MENU** 버튼을 누르고 있으면 설정과 무관하게 ConfigApp(`--bootmenu`)이 표시됩니다.
- **Rockbox 화면 및 절전 설정:**
  - `절전 (백라이트 꺼짐 후)` (`HAVE_LCD_SLEEP_SETTING`) 및 `절전 백라이트 꺼짐 밝기`, 잠금 화면 백라이트 완전 끄기 기능을 지원합니다.

---

## 관련 저장소
Rockbox: https://github.com/norocoral/rockbox

---

## 라이선스

Innioasis 사와는 제휴 관계가 아닙니다.

## 투명성 안내
본 저장소의 리버스 엔지니어링, 드라이버 및 부팅 코드는 AI(Google Antigravity)의 지원을 받아 작성되었습니다.
