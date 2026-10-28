<img width="1440" height="1080" alt="hero1" src="https://github.com/user-attachments/assets/10e1d348-e75f-4271-a893-6ff63120375e" />

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
| 키 잠금 모드 | 없음 | 없음 | **어드밴스드 키 락 기반 자동 HOLD** |
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
| 센터 버튼 | 짧게 누름 (화면 꺼짐 시) | 잠금 해제 및 화면 켜기 (클릭 입력은 안전하게 무시됨) |
| 센터 버튼 | 길게 누름 (USB 화면에서 1초) | 대용량 저장소 마운트 해제 및 충전 전용 모드로 전환 |
| 메뉴 버튼 | 짧게 누름 | 상위 메뉴로 이동 / 뒤로 가기 |
| 메뉴 버튼 | 길게 누름 | Rockbox 퀵 스크린 설정 열기 |
| 재생 버튼 | 짧게 누름 | 재생 / 일시정지 전환 |
| 재생 버튼 | 길게 누름 | 재생 정지 |
| 좌측 버튼 | 짧게 누름 | 재생 시간 $\ge 3$초 시 곡 처음으로 되감기, $< 3$초 시 이전 트랙으로 이동 |
| 좌측 버튼 | 길게 누름 | 뒤로 탐색(리드 스캔) |
| 우측 버튼 | 짧게 누름 | 다음 트랙으로 이동 |
| 우측 버튼 | 길게 누름 | 앞으로 탐색(고속 탐색) |
| 측면 버튼 | 누름 | 볼륨 올림 / 내림 (1 dB 단위) |
| 헤드폰 리모컨 (MFB) | 인라인 클릭 | 단일/더블/트리플 클릭 및 길게 누르기 동작 (설정에서 변경 가능) |

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