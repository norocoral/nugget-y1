<img width="1440" height="1080" alt="herokr" src="https://github.com/user-attachments/assets/e2ccc92e-e80b-4137-9537-2affff1bcf59" />

🌐 **한국어** | [English](README.md)

# Nugget for Innioasis Y1

[![Platform](https://img.shields.io/badge/Platform-MediaTek%20MT6572-orange.svg)](#하드웨어-사양)

Nugget for Y1은 Innioasis Y1 디지털 오디오 플레이어를 위한 오픈 소스 커스텀 펌웨어입니다. 안드로이드를 완전히 걷어내고, 네이티브 호스티드 록박스(Hosted Rockbox)로 직접 부팅되는 미니멀 다운스트림 리눅스 커널(3.4.67)로 대체합니다.

---

## 기술 사양 비교

| 기능 | 순정 안드로이드 | 안드로이드 위 Rockbox | Nugget for Y1 (호스티드 리눅스) |
| :--- | :--- | :--- | :--- |
| 오디오 재생 배터리 지속 시간 | ~30시간 | ~20시간 | **~50시간** |
| 대기 전류 | 높음 (~120-140 mA) | 높음 (~140-180 mA) | **< 5 mA (200시간 이상)** |
| 콜드 부팅 시간 | ~40초 | ~45초 | **< 6초** |
| 샘플 레이트 지원 | AudioFlinger를 통한 리샘플링 | 리샘플링 | **네이티브 비트 퍼펙트 44.1 및 48 kHz** |
| 오디오 출력 경로 | 표준 안드로이드 HAL | 표준 안드로이드 HAL | **직접 MT6572 /dev/eac 제어** |
| 헤드폰 앰프 게인 | 전체 | 전체 | **전체 (+12dB 아날로그 헤드룸)** |
| 볼륨 스케일 | OS 마스터 볼륨 | 다중 출력 환경에서 불안정 | **Rockbox 로그 스케일 (-74 dB ~ +6 dB)** |
| 디스플레이 모드 | 480x360 전용 | 480x360 전용 | **480x360 네이티브 + 320x240 클래식 모드** |
| iPod 테마 호환성 | 미지원 | 480x360 리소스 필요 | **iPod Classic 테마 네이티브 지원** |
| 키 잠금 모드 | 없음 | 없음 | **홀드 라이트 및 고급 키 잠금 지원 iPod Classic HOLD** |
| USB 연결 | 안드로이드 MTP / 대용량 저장소 | 안드로이드 대용량 저장소 | **대용량 저장소(Mass Storage)** |

---

## 기술 문서

상세 서브시스템 사양, 아키텍처 다이어그램 및 리버스 엔지니어링 결과는 [`hosted_linux_port/docs/`](hosted_linux_port/docs/)에서 확인하실 수 있습니다:
- [기술 분석 결과](hosted_linux_port/docs/TECHNICAL_FINDINGS.md): 하드웨어 인터페이스, `/dev/eac` 오디오 드라이버, PMIC 레지스터, 전력 튜닝, 부팅 최적화 및 입력 이벤트 세부 특성.
- [시스템 아키텍처](hosted_linux_port/docs/ARCHITECTURE.md): 아키텍처 블록 다이어그램, 부팅 시퀀스 흐름, 스토리지 레이아웃 및 HAL 매핑.
- [오디오 서브시스템](hosted_linux_port/docs/AUDIO_SUBSYSTEM.md): MediaTek EAC 드라이버, 팝 노이즈 제거 메커니즘, 비트 퍼펙트 클록 전환 및 게인 스테이징.
- [전원 관리](hosted_linux_port/docs/POWER_MANAGEMENT.md): 대기 중 배터리 소모 해결(< 5mA 대기), CPU 주파수 거버너, 코어 핫플러그 오프라인 및 Always-On Display (AOD).

---

## 설치 방법

### 권장: Nugget Y1 Tool
자동화 설치 도구:
https://github.com/norocoral/nugget-y1-tool

### 수동 설치 방법
1. `boot_rockbox.img` 및 `rockbox_sdcard.zip`을 준비합니다.
2. 다음 도구를 사용하여 `boot_rockbox.img`를 `boot` 파티션에 플래시합니다:
   - SP Flash Tool: MT6572 산란 텍스트(scatter) 로드, `BOOTIMG` 체크, `boot_rockbox.img` 선택 후 Download 클릭, 그리고 볼륨 다운 버튼을 누른 채로 Y1을 연결합니다.
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

### 빌드 명령
```bash
cd hosted_linux_port
./build.sh
```

빌드 스크립트 실행 동작:
1. `!stock_rom/boot.img`에서 순정 커널을 추출합니다(없는 경우).
2. ARM 하드 플로트 GCC를 사용하여 C 언어로 작성된 커스텀 PID 1 `/init` 및 `unmute_eac` 유틸리티를 컴파일합니다.
3. 스플래시 화면 PNG 이미지를 32비트 원시 프레임버퍼 파일로 변환합니다.
4. Innioasis Y1용 호스티드 록박스를 컴파일합니다(`build_y1/`).
5. `.rockbox` SD 카드 리소스를 `rockbox_sdcard.zip`으로 패키징합니다.
6. 최소 구성의 initramfs 램디스크를 빌드하고 MediaTek 헤더가 포함된 플래시용 `boot_rockbox.img`를 생성합니다.

*참고: 오픈 소스 라이선스 규정을 준수하기 위해 MediaTek 독점 바이너리(`bt_blobs/`, `stock_unpacked/`, `logo.bin`)는 Git 추적에서 제외되어 있습니다.*

---

## 버튼 조작 가이드

iPod Classic과 동일한 내비게이션 체계에 전용 하드웨어 기능이 추가되었습니다:

| 조작부 | 입력 방식 | 기능 설명 |
| :--- | :--- | :--- |
| 클릭 휠 | 회전 | 목록 스크롤 / 재생 화면(WPS)에서 볼륨 조절 |
| 센터 버튼 | 짧게 누름 | 메뉴 항목 선택 / 활성화 |
| 센터 버튼 | 길게 누름 (1초) | 현재 트랙/파일의 컨텍스트 메뉴 열기 |
| 센터 버튼 | 길게 누름 (USB 화면에서 1초) | 대용량 저장소 마운트 해제 및 충전 전용 모드로 전환 |
| 메뉴 버튼 | 짧게 누름 | 상위 메뉴로 이동 / 뒤로 가기 (120ms 즉각 반응) |
| 메뉴 버튼 | 더블 클릭 | 키 잠금(HOLD 모드) 활성화 / 해제 토글 |
| 메뉴 버튼 | 길게 누름 (280ms) | Rockbox 퀵 스크린 설정 열기 |
| 재생 버튼 | 짧게 누름 | 재생 / 일시정지 전환 |
| 재생 버튼 | 길게 누름 | 재생 정지 |
| 좌측 버튼 | 짧게 누름 | 재생 시간 $\ge 3$초 시 곡 처음으로 되감기, $< 3$초 시 이전 트랙으로 이동 |
| 좌측 버튼 | 길게 누름 | 뒤로 탐색(리드 스캔) |
| 우측 버튼 | 짧게 누름 | 다음 트랙으로 이동 |
| 우측 버튼 | 길게 누름 | 앞으로 탐색(고속 탐색) |
| 측면 버튼 | 누름 | 볼륨 올림 / 내림 (1 dB 단위) |
| 헤드폰 리모컨 (MFB) | 인라인 클릭 | 단일/더블/트리플 클릭 및 길게 누르기 동작 (설정에서 변경 가능) |

---

### iPod Classic HOLD 및 백라이트 제어 서브시스템

Nugget for Y1은 Y1의 하드웨어 특성에 맞춰 정밀하게 구현된 iPod Classic 스타일의 HOLD 및 백라이트 시스템을 제공합니다:

- **HOLD 토글 (잠금 / 잠금 해제):**
  - 언제든지 **MENU** 버튼을 빠르게 두 번 누르면(더블 클릭) HOLD 모드가 켜지거나 꺼집니다.
  - 잠금 상태에서는 350ms의 넉넉한 더블 클릭 판정 창을 두어 주머니 속 해제가 직관적이며, 잠금 해제 상태에서는 단일 클릭이 120ms만에 즉각 실행되어 메뉴 뒤로 가기 조작 시 답답함이나 딜레이가 없습니다.
- **잠금 상태 (HOLD 활성화):**
  - **오작동 방지:** 주머니 속 오작동을 방지하기 위해 모든 버튼과 휠 조작이 잠깁니다 (*고급 키 잠금(Advanced Key Lock)* 설정을 통해 볼륨 조절이나 재생/일시정지, 곡 넘김을 예외적으로 허용할 수 있습니다).
  - **홀드 라이트 (Hold Light) 설정:**
    - `꺼짐 (Off)` *(기본값)*: 완전한 주머니 절전 모드입니다. 버튼이나 휠을 조작해도 화면이 켜지지 않고 꺼진 상태를 유지합니다.
    - `켜짐 (On 또는 지정 밝기)`: 조작 시 명령은 실행되지 않으며, 현재 시각이나 재생 중인 곡 정보를 확인할 수 있도록 백라이트만 켜집니다.
  - **잠금 모드 백라이트 꺼짐 시간 (Backlight on Lock Timeout):** 최대 **5시간**까지 세부 설정이 가능합니다(꺼짐, 항상 켬, 1초~45초, 1분~45분, 1시간~5시간). 잠금 모드로 전환되는 즉시 설정된 잠금 타임아웃과 밝기가 동적으로 적용됩니다.
  - **잠금 모드 백라이트 규칙 (Backlight on Lock Rules):** 잠금 상태에서 화면을 유지할 조건을 지정합니다 (`Always`, `재생 중에만`, `WPS 화면에서만`).
- **잠금 해제 상태 (일반 동작):**
  - **첫 번째 키 입력 시 백라이트만 켜기 (First Keypress Enables Backlight Only):**
    - `아니오 (No)` *(기본값)*: 화면이 꺼져 있을 때 버튼(예: 다음 곡, 재생/일시정지)을 누르면 즉시 명령을 수행하면서 동시에 디스플레이 백라이트를 켭니다.
    - `예 (Yes)`: 화면이 꺼져 있을 때 첫 번째 버튼 입력은 오직 화면 백라이트만 켜고 명령을 차단합니다. 이후 화면이 켜진 상태에서 버튼을 눌러야 명령이 실행됩니다.
  - **일반 백라이트 꺼짐 타이머:** 일반 조작 중에는 설정된 표준 백라이트 꺼짐 시간(Backlight Timeout)에 따라 유휴 시 디스플레이가 절전됩니다.

---

### 듀얼 부팅 서브시스템 및 부트로더 메뉴 (Dual-Boot & Bootloader Menu)

Nugget은 내부 eMMC의 안드로이드 파티션(`SYSTEM`, `DATA`, `CACHE`)을 손상시키지 않고 보존하므로, 안드로이드와 Nugget 간의 완벽한 듀얼 부팅을 지원합니다:

- **부트로더 메뉴 (Bootloader Menu - Approach 2):**
  - 기기 전원을 켤 때 그 어떤 프로세스보다 먼저 로드되는 그래픽 부트로더 메뉴입니다.
  - 전용 고해상도 그래픽 스프라이트가 적용되어 있습니다:
    - **Pick 1: Nugget** — 외장 MicroSD 카드의 Rockbox 환경으로 즉시 부팅합니다.
    - **Pick 2: Android** — 안드로이드 / 리커버리 모드로 직접 재부팅합니다.
    - **Pick 3: Shut down** — 시스템을 안전하게 종료(전원 끄기)합니다.
  - **조작법:** 클릭 휠을 돌리거나 **좌/우 버튼**을 눌러 선택 화살표를 이동하고, **센터 버튼** 또는 **재생 버튼**을 눌러 선택을 확정합니다.
  - **하드웨어 강제 진입:** **부팅 중 MENU 버튼을 길게 누르고 있으면 설정 활성화 여부와 관계없이 항상 부트로더 메뉴가 표시**됩니다.
  - **설정 옵션:** `설정 -> Nugget -> 부트로더 메뉴` (`예` / `아니오`)에서 켜거나 끌 수 있습니다. 비활성화 시 MENU 버튼을 누르고 있지 않으면 바로 Nugget으로 고속 부팅합니다.
- **Rockbox 내 안드로이드 재부팅 (Reboot to Android - Approach 3):**
  - Rockbox 사용 중 언제든지 `설정 -> Nugget -> 안드로이드로 재부팅`을 선택하면 캐시를 안전하게 저장하고 즉시 안드로이드로 재부팅합니다.

---

## 크레딧
https://github.com/nuxcodes/rockpod

---

## 라이선스

GNU 일반 공중 사용 허가서 v2(GPL-2.0)에 따라 오픈 소스 [Rockbox](https://www.rockbox.org) 프로젝트를 기반으로 제작되었습니다.
Innioasis 사와는 제휴 관계가 아닙니다.

## 투명성 안내
코드 개발 과정은 투명하게 공개되어야 한다고 믿습니다.
본 프로젝트의 리버스 엔지니어링, 드라이버 및 부팅 코드는 AI(Google Antigravity)를 통해 생성되었습니다.

본 프로젝트의 리버스 엔지니어링, 드라이버 및 부팅 코드는 AI(Google Antigravity)를 통해 생성되었습니다.