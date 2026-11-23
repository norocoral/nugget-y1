CROSS_COMPILE ?= arm-linux-gnueabihf-
CC            := $(CROSS_COMPILE)gcc
STRIP         := $(CROSS_COMPILE)strip

LIBY1_DIR := ../liby1
STAGE_DIR ?= ../dist/rockbox_sdcard
CFLAGS    := -O2 -Wall -Wextra -I$(LIBY1_DIR)/include -Ithird_party
LDFLAGS   := $(LIBY1_DIR)/build/liby1.a -lpthread -lm

TARGET    := build/configapp
SRCS      := src/main.c src/ui_gfx.c
OBJS      := $(patsubst src/%.c,build/%.o,$(SRCS))

.PHONY: all clean stage

all: $(TARGET)

build:
	mkdir -p build

$(LIBY1_DIR)/build/liby1.a:
	$(MAKE) -C $(LIBY1_DIR) CROSS_COMPILE=$(CROSS_COMPILE)

build/%.o: src/%.c src/ui_gfx.h src/cfg_registry.h src/hangul_ime.h src/lang.h $(LIBY1_DIR)/include/liby1.h | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(LIBY1_DIR)/build/liby1.a
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	$(STRIP) --strip-unneeded $@

stage: $(TARGET)
	mkdir -p $(STAGE_DIR)/.system/configapp/fonts
	mkdir -p $(STAGE_DIR)/.system/configapp/icons
	cp $(TARGET) $(STAGE_DIR)/.system/configapp/configapp
	cp assets/fonts/*.ttf $(STAGE_DIR)/.system/configapp/fonts/
	cp assets/icons/*.svg $(STAGE_DIR)/.system/configapp/icons/

clean:
	rm -rf build
