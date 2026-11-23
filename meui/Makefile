CROSS_COMPILE ?= arm-linux-gnueabihf-
CC            := $(CROSS_COMPILE)gcc
STRIP         := $(CROSS_COMPILE)strip

LIBY1_DIR  := ../liby1
STAGE_DIR  ?= ../dist/rockbox_sdcard
FFMPEG_DIR := third_party/ffmpeg_arm_static
CFLAGS     := -O2 -Wall -Wextra -I$(LIBY1_DIR)/include -Ithird_party -I$(FFMPEG_DIR)/include
LDFLAGS    := $(LIBY1_DIR)/build/liby1.a $(FFMPEG_DIR)/lib/libavformat.a $(FFMPEG_DIR)/lib/libavcodec.a $(FFMPEG_DIR)/lib/libswresample.a $(FFMPEG_DIR)/lib/libavutil.a -lpthread -lm

TARGET    := build/meui
SRCS      := src/main.c src/ui_gfx.c src/ipod_db.c src/audio_player.c src/album_art.c src/photos.c
OBJS      := $(patsubst src/%.c,build/%.o,$(SRCS))

.PHONY: all clean stage

all: $(TARGET)

build:
	mkdir -p build

$(LIBY1_DIR)/build/liby1.a:
	$(MAKE) -C $(LIBY1_DIR) CROSS_COMPILE=$(CROSS_COMPILE)

build/%.o: src/%.c src/ui_gfx.h src/ipod_db.h src/audio_player.h src/album_art.h src/photos.h src/lang.h $(LIBY1_DIR)/include/liby1.h | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(LIBY1_DIR)/build/liby1.a Makefile
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	$(STRIP) --strip-unneeded $@

stage: $(TARGET)
	mkdir -p $(STAGE_DIR)/.meui/fonts
	mkdir -p $(STAGE_DIR)/.meui/icons
	rm -rf $(STAGE_DIR)/.meui/lib
	cp $(TARGET) $(STAGE_DIR)/.meui/meui
	cp assets/fonts/*.ttf $(STAGE_DIR)/.meui/fonts/
	cp assets/icons/*.svg $(STAGE_DIR)/.meui/icons/

clean:
	rm -rf build
