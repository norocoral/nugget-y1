🌐 **한국어** | [English](TECHNICAL.md)

# 미니멀 리눅스 기반 호스티드 Rockbox (Innioasis Y1)

이 디렉터리는 Innioasis Y1을 위한 모듈식 분리형 빌드 시스템, 최소 구성 리눅스 initramfs, 독립형 하드웨어 추상화 계층(`liby1`) 및 패키징 도구를 포함하고 있습니다. 플래시용 부트 이미지에는 리눅스 커널과 초기 기동 로직이 담겨 있으며, Rockbox 실행 파일, 하드웨어 라이브러리 및 시스템 자산은 외장 MicroSD 카드에 저장됩니다.

---

## 기술 문서 및 가이드 목록

상세 서브시스템 사양, 개발자 매뉴얼 및 리버스 엔지니어링 기록:
- **[iPod 5.5G USB 모드 및 동기화 엔진](docs/kr/IPOD_SYNC.md)** ([English](docs/IPOD_SYNC.md)): 기기 자체 온디바이스 양방향 iTunes ↔ Rockbox 동기화, Apple 5.5G 하드웨어 스푸핑, TagCache .tcd 생성, .ithmb 앨범아트 추출, 비디오 DB 및 듀얼코어 터보 잠금 안내.
- **[모듈식 빌드 시스템 가이드](docs/kr/BUILD_GUIDE.md)** ([English](docs/BUILD_GUIDE.md)): 분리형 아키텍처, 터미널 그래픽 메뉴(`--gui`), `build.conf` 설정 레퍼런스, 명령행 플래그, 독립형 SDK 빌드 및 플래시 절차.
- **[`liby1` 개발자 매뉴얼 및 API 가이드](docs/kr/LIBY1_GUIDE.md)** ([English](docs/LIBY1_GUIDE.md)): 디스플레이, 오디오, 입력, 전원, CPU, FM 라디오, USB 가젯을 제어하는 전체 C API 레퍼런스.
- **[시스템 아키텍처](docs/kr/ARCHITECTURE.md)** ([English](docs/ARCHITECTURE.md)): 구성 요소 다이어그램, 부팅 시퀀스 흐름, 스토리지 계층 구조(`.system` vs `.rockbox`) 및 HAL 매핑.
- **[기술 분석 결과](docs/kr/TECHNICAL_FINDINGS.md)** ([English](docs/TECHNICAL_FINDINGS.md)): 리버스 엔지니어링된 하드웨어 인터페이스, EAC 오디오 드라이버, PMIC 레지스터, 전력 튜닝, 입력 특이사항 및 부팅 지연 시간 분석.
- **[오디오 서브시스템](docs/kr/AUDIO_SUBSYSTEM.md)** ([English](docs/AUDIO_SUBSYSTEM.md)): `/dev/eac` 인터페이스, 비트 퍼펙트 44.1/48kHz 재생, 팝 노이즈 제거 및 게인 스테이징.
- **[전원 관리](docs/kr/POWER_MANAGEMENT.md)** ([English](docs/POWER_MANAGEMENT.md)): 유휴 상태 배터리 소모 해결, CPU 주파수 거버너, 코어 오프라인 및 Always-On Display.

---

## 디렉터리 구조

- `docs/`: 기술 사양서, 아키텍처 설계 문서, 빌드 가이드 및 하드웨어 분석 기록.
- `liby1/`: 독립형 C 하드웨어 추상화 계층 (`liby1.a`, `liby1.so`, `include/liby1.h`, 예제 애플리케이션).
- `initramfs/`: C 언어로 작성된 PID 1 `/init`. 부팅 CPU 부스트, 반응형 MicroSD 탐색, 프레임버퍼 상태 화면 표시, 부트로더 메뉴 및 Rockbox/커스텀 앱 실행을 담당.
- `audio/`: 독립형 `unmute_eac.c` 테스트 유틸리티 및 `init_audio.sh` 설정 스크립트.
- `splash/`: 소스 PNG 상태 화면 이미지 (`boot.png`, `connect.png`, `errconnect.png`, `error.png`, `sdcard.png`).
- `bootl_image_assets/`: 부트로더 메뉴 PNG 아이콘 (Rockbox, MeUI, User Software, Android, Shut Down, USB Mass Storage, Linux Terminal).
- `Themify2/`: SD 카드 배포용 번들 커스텀 Rockbox 테마, 글꼴, 아이콘 및 코덱.
- `tools/`: 모듈식 빌드 도구, 터미널 GUI 설정기(`build_tui.py`), 스플래시 이미지 변환기(`convert_splash.py`), MTK 부트 이미지 패커(`mkbootimg_mtk.py`), CPIO 램디스크 생성기(`build_initramfs.py`).
- `build.conf`: 중앙 빌드 구성 파일.
- `rootfs/`: (빌드 시 생성) 최소 initramfs 램디스크의 기본 골격.
- `stock_unpacked/`: (빌드 시 생성) 추출된 벤더 커널(`kernel_mtk.bin`), Git 추적 제외.
- `bt_blobs/`: MediaTek 블루투스 커널 모듈 및 펌웨어 패치, Git 추적 제외.

---

## 소스 코드 빌드

### 사전 필수 패키지 (Ubuntu / Debian / WSL)
```bash
sudo apt update
sudo apt install -y build-essential gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf python3 python3-pip zip curl ccache
```

### 대화형 터미널 그래픽 메뉴 (TUI)
내장된 터미널 메뉴를 통해 빌드 구성 요소를 시각적으로 손쉽게 설정할 수 있습니다:
```bash
./build.sh --gui
```
메뉴에서 방향키와 스페이스바로 빌드 컴포넌트(`liby1`, rootfs, 부트 이미지, Rockbox, SD 패키징, 커스텀 앱)를 켜거나 끄고, `B`를 눌러 저장과 동시에 빌드를 시작할 수 있습니다.

### 명령행 빌드
```bash
# 전체 표준 빌드 (Rockbox + 부트 이미지 + SD 카드 패키지)
./build.sh

# 독립형 리눅스 SDK 빌드 (Rockbox 제외)
./build.sh --no-rockbox

# liby1만 빌드 (liby1.a, liby1.so, hello_y1 예제)
./build.sh --only-liby1

# rootfs 및 boot_nugget.img만 빌드
./build.sh --only-bootimg

# 개발한 커스텀 C 바이너리를 /.system/app 에 패키징
./build.sh --no-rockbox --custom-app /path/to/my_arm_app
```

---

## SD 카드 디렉터리 레이아웃

모든 시스템, 하드웨어 및 개발자 파일은 `/mnt/sdcard/.system/`(`/.system`에 심볼릭 링크 연결)에 저장되어 Rockbox 디렉터리를 깨끗하게 유지합니다:

```text
/mnt/sdcard/
|-- .system/                      # 시스템 및 하드웨어 SDK 파일
|   |-- lib/                      # liby1.so, liby1video.so
|   |-- include/                  # liby1.h
|   |-- bin/                      # ffmpeg
|   |-- bt/                       # 블루투스 커널 모듈 및 펌웨어
|   |-- bt_paired.dat             # 페어링 기기 데이터베이스
|   |-- ramdisk_android.cpio.gz   # 듀얼 부팅용 안드로이드 램디스크
|   |-- y1_debug.txt              # 하드웨어 진단 로그
|   `-- app                       # 부팅 시 실행 가능한 독립형 애플리케이션
|-- .rockbox/                     # Rockbox 바이너리, 코덱, 테마, 글꼴
`-- Videos/
```

---

## 진단 로깅

Y1의 진단 로깅 기능은 기본적으로 비활성화되어 있습니다. **설정 -> Nugget -> 디바이스 -> 진단 로깅**을 켜면 초기화, 디스플레이, 오디오, 블루투스, 버튼 및 절전 진단 정보가 `/mnt/sdcard/.system/y1_debug.txt`에 기록됩니다. 이 설정은 `.system/system.cfg`(또는 `.rockbox/config.cfg`)에 저장되며, 부팅 시 `/init`이 이를 읽어 시작 로그 출력 여부를 결정합니다.

---

## 하드웨어 컨트롤 가이드

| 조작부 | 입력 방식 | 기능 설명 |
| :--- | :--- | :--- |
| 클릭 휠 | 회전 | 목록 스크롤 / 재생 화면(WPS)에서 볼륨 조절 |
| 센터 버튼 | 짧게 누름 | 메뉴 항목 선택 / 활성화 |
| 센터 버튼 | 길게 누름 (1초) | 현재 트랙/파일의 컨텍스트 메뉴 열기 |
| 센터 버튼 | 길게 누름 (USB 화면에서 1초) | 대용량 저장소 마운트 해제 및 충전 전용 모드로 전환 |
| 메뉴 버튼 | 짧게 누름 | 상위 메뉴로 이동 / 뒤로 가기 (120ms 즉각 반응) |
| 메뉴 버튼 | 더블 클릭 | 키 잠금(HOLD 모드) 활성화 / 해제 토글 |
| 메뉴 버튼 | 길게 누름 (280ms) | Rockbox 퀵 스크린 설정 열기 |
| 메뉴 버튼 | 부팅 중 누르고 있기 | **부트로더 메뉴 강제 열기** (타임아웃 무시) |
| 재생 버튼 | 짧게 누름 | 재생 / 일시정지 전환 |
| 재생 버튼 | 길게 누름 | 재생 정지 |
| 좌측 버튼 | 짧게 누름 | 재생 시간 $\ge 3$초 시 곡 처음으로 되감기, $< 3$초 시 이전 트랙으로 이동 |
| 좌측 버튼 | 길게 누름 | 뒤로 탐색 |
| 우측 버튼 | 짧게 누름 | 다음 트랙으로 이동 |
| 우측 버튼 | 길게 누름 | 앞으로 탐색 |
| 헤드폰 리모컨 (MFB) | 인라인 클릭 | 단일/더블/트리플 클릭 및 길게 누르기 동작 (설정에서 구성 가능) |

---

## iPod Classic HOLD 및 백라이트 제어 서브시스템

Nugget for Y1은 하드웨어 홀드 스위치가 없는 Y1 환경에서 iPod Classic의 직관적인 잠금 및 백라이트 경험을 구현합니다:

- **HOLD 토글 (잠금 / 잠금 해제):**
  - 언제든지 **MENU** 버튼을 빠르게 두 번 누르면(더블 클릭) HOLD 모드가 전환됩니다.
  - 잠금 상태에서는 350ms의 넉넉한 더블 클릭 판정 창을 두어 오작동 없이 편리하게 잠금을 해제할 수 있으며, 잠금 해제 상태에서는 단일 클릭이 120ms만에 즉각 실행되어 쾌적한 메뉴 탐색을 제공합니다.
- **잠금 상태 (HOLD 활성화):**
  - **입력 보호:** 주머니 속 오작동을 차단하기 위해 모든 휠 스크롤과 버튼 입력이 잠깁니다 (*고급 키 잠금*을 통해 볼륨 조절, 재생/일시정지, 곡 넘김을 선택적으로 허용 가능).
  - **홀드 라이트 (Hold Light) 설정:**
    - `꺼짐 (Off)` *(기본값)*: 완전한 주머니 절전 모드로 버튼을 눌러도 화면이 켜지지 않습니다.
    - `켜짐 (On 또는 지정 밝기)`: 버튼 입력 시 명령은 실행되지 않으며 백라이트만 켜져 재생 상태 및 시각을 확인할 수 있습니다.
  - **잠금 모드 백라이트 꺼짐 시간 (Backlight on Lock Timeout):** 꺼짐부터 최대 **5시간**까지 세밀하게 설정할 수 있으며, 잠금 진입 즉시 잠금 타임아웃과 밝기가 동적으로 적용됩니다.
  - **잠금 모드 백라이트 규칙 (Backlight on Lock Rules):** 잠금 상태에서 화면을 유지할 조건을 지정합니다 (`Always`, `재생 중에만`, `WPS 화면에서만`).
- **잠금 해제 상태 (일반 동작):**
  - **첫 번째 키 입력 시 백라이트만 켜기 (First Keypress Enables Backlight Only):**
    - `아니오 (No)` *(기본값)*: 화면이 꺼져 있을 때 버튼(예: 다음 곡, 재생/일시정지)을 누르면 즉시 명령을 수행하면서 동시에 디스플레이 백라이트를 켭니다.
    - `예 (Yes)`: 화면이 꺼져 있을 때 첫 번째 버튼 입력은 오직 화면 백라이트만 켜고 명령을 차단합니다. 이후 화면이 켜진 상태에서 버튼을 눌러야 명령이 실행됩니다.
  - **일반 백라이트 꺼짐 타이머:** 일반 조작 중에는 표준 백라이트 꺼짐 시간 설정에 따라 유휴 시 디스플레이가 절전됩니다.
