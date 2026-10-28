🌐 **한국어** | [English](../ARCHITECTURE.md)

# 시스템 아키텍처: Innioasis Y1 호스티드 리눅스 포트

이 문서는 다운스트림 미니멀 리눅스 기반 Innioasis Y1 Rockbox 포트의 소프트웨어 아키텍처, 프로세스 모델, 부팅 수명 주기 및 스토리지 레이아웃에 대해 상세히 설명합니다.

---

## 1. 시스템 블록 다이어그램

```mermaid
flowchart TD
    subgraph HW["하드웨어 계층"]
        CPU["MT6572 듀얼 Cortex-A7 (1.2 GHz)"]
        PMIC["MT6323 PMIC 및 DAC"]
        LCD["2.8인치 LCD (480x360 BGRA)"]
        SD["MicroSD 카드 (mmcblk1p1)"]
        BTN["키패드 및 정전식 휠"]
        JACK["3.5mm 헤드폰 잭 (accdet)"]
    end

    subgraph KERNEL["다운스트림 리눅스 커널 3.4.67"]
        K_EAC["/dev/eac 오디오 디바이스"]
        K_FB["/dev/fb0 프레임버퍼"]
        K_EV["/dev/input/event* 노드"]
        K_MMC["mmc_block 및 파일시스템 드라이버"]
        K_CPU["cpufreq 및 핫플러그 서브시스템"]
    end

    subgraph INITRAMFS["초기 램디스크 (PID 1 /init)"]
        INIT_BOOST["부팅 CPU 클록 부스트 (1.2 GHz 듀얼 코어)"]
        INIT_MDEV["mdev 동적 디바이스 노드 생성"]
        INIT_STORAGE["반응형 MicroSD 마운트 (/mnt/sdcard)"]
        INIT_SPLASH["직접 프레임버퍼 상태 화면 표시"]
        INIT_EXEC["execve Rockbox 바이너리 실행"]
    end

    subgraph ROCKBOX["호스티드 Rockbox 프로세스"]
        RB_CORE["Rockbox 코어 엔진 및 버퍼 매니저"]
        RB_PCM["PCM 드라이버 (/dev/eac 인터페이스)"]
        RB_LCD["디스플레이 드라이버 (480x360 네이티브 / 320x240 클래식)"]
        RB_KEY["입력 드라이버 (휠, 택트 키, MFB 리모컨)"]
        RB_PWR["전원 관리 및 디스플레이 절전 제어"]
    end

    HW --> KERNEL
    KERNEL --> INITRAMFS
    INITRAMFS --> ROCKBOX
    ROCKBOX --> KERNEL
```

---

## 2. 부팅 시퀀스 및 프로세스 라이프사이클

```mermaid
sequenceDiagram
    autonumber
    participant LK as MediaTek Little Kernel (LK)
    participant K as 리눅스 커널 (3.4.67)
    participant I as PID 1 (/init C 프로그램)
    participant SD as MicroSD 카드 (mmcblk1p1)
    participant RB as Rockbox 실행 파일

    LK->>K: MTK ATAGs와 함께 실행 권한 이양
    K->>K: SoC 드라이버, 클록, 메모리 초기화
    K->>I: initramfs에서 /init 실행
    I->>I: boost_cpu_for_boot() -> 두 코어 모두 1.2 GHz 성능 모드 고정
    I->>I: /proc, /sys, /dev 마운트
    I->>I: /etc/boot.raw 데이터를 /dev/fb0에 직접 기록
    I->>SD: mmcblk1p1 반응형 폴링 (10ms 주기)
    alt MicroSD 카드 감지됨
        I->>SD: mmcblk1p1을 /mnt/sdcard 경로에 마운트
        I->>I: /.rockbox 심볼릭 링크 생성
        I->>RB: execve("/mnt/sdcard/.rockbox/rockbox")
    else 카드가 없거나 읽을 수 없음
        I->>I: /etc/sdcard.raw를 /dev/fb0에 출력하고 USB 대용량 저장소 모드 초기화
    end
    RB->>RB: 메모리 버퍼, 설정값, 코덱 초기화
    RB->>RB: 테마 및 스킨 적용
    RB->>I: y1_boot_boost_finish() 호출 -> CPU를 사용자 설정 프로필로 전환
    RB->>RB: 메인 이벤트 처리 및 음악 재생 루프 진입
```

---

## 3. 스토리지 계층 구조 및 파티션 구성

하드웨어는 내부 eMMC(`mmcblk0`)와 외부 MicroSD(`mmcblk1`)로 구성됩니다.

### 3.1 파티션 구성 체계
- `mmcblk0` (내부 eMMC): 순정 MediaTek 파티션(`BOOTIMG`, `RECOVERY`, `LOGO`, `SYSTEM`, `CACHE`, `DATA`)이 포함되어 있습니다. Rockbox는 `BOOTIMG` 파티션만 `boot_rockbox.img`(최대 6 MiB)로 덮어씁니다. eMMC의 나머지 파티션은 마운트되지 않은 상태로 유지됩니다.
- `mmcblk1p1` (외장 MicroSD): FAT32, exFAT, NTFS 또는 ext4 형식으로 포맷됩니다. Rockbox의 주 루트 스토리지 드라이브 역할을 수행합니다:
  ```text
  /mnt/sdcard/
  |-- .rockbox/
  |   |-- rockbox          # ELF ARM 하드 플로트 실행 파일
  |   |-- codecs/          # 오디오 디코더 (.codec)
  |   |-- rocks/           # 플러그인 및 뷰어 (.rock, .ovl)
  |   |-- fonts/           # UI 글꼴 (.fnt, .fnticons)
  |   |-- themes/          # 테마 설정 (.cfg)
  |   |-- wps/             # 재생 화면 스킨 (.wps, .sbs)
  |   |-- config.cfg       # 영구 저장 설정 파일
  |   `-- boot.raw         # 커스텀 부팅 스크린
  `-- Music/               # 음악 라이브러리
  ```

---

## 4. 하드웨어 추상화 계층 (HAL) 매핑

| 서브시스템 | 소스 파일 경로 | 기술적 역할 |
| :--- | :--- | :--- |
| PCM 오디오 | [`firmware/target/hosted/innioasis/y1/pcm-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/pcm-y1.c) | `/dev/eac`와 직접 통신. 44.1/48kHz 클록 전환 및 팝 노이즈 억제 처리. |
| 오디오 제어 | [`firmware/target/hosted/innioasis/y1/audio-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/audio-y1.c) | 헤드폰 앰프 아날로그 게인(+12dB) 및 음소거 GPIO 라인 제어. |
| CPU 주파수 | [`firmware/target/hosted/innioasis/y1/cpufreq-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/cpufreq-y1.c) | 거버너 관리, 코어 오프라인 처리, 화면 꺼짐 스로틀링 및 부팅 부스트 종료 처리. |
| LCD / 디스플레이 | [`firmware/target/hosted/innioasis/y1/lcd-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/lcd-y1.c) | 프레임버퍼 출력(`/dev/fb0`), 480x360 네이티브 / 320x240 스트레치 스케일링, 1 FPS AOD 리프레시. |
| 키패드 및 휠 | [`firmware/target/hosted/innioasis/y1/button-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/button-y1.c) | 클릭 휠 및 버튼에서 발생하는 `evdev` 이벤트를 Rockbox `BUTTON_*` 코드로 변환. |
| 전원 및 배터리 | [`firmware/target/hosted/innioasis/y1/power-y1.c`](file:///c:/Users/dakirb/Documents/rom/!rockbox_fromsource/rockbox-master/rockbox-master/firmware/target/hosted/innioasis/y1/power-y1.c) | MT6323 sysfs 인터페이스로부터 배터리 전압 및 충전 상태 수집. |