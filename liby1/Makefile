# Standalone Makefile for liby1 (Innioasis Y1 Hardware Layer)
CROSS_COMPILE ?= arm-linux-gnueabihf-
CC            := $(CROSS_COMPILE)gcc
AR            := $(CROSS_COMPILE)ar
STRIP         := $(CROSS_COMPILE)strip

FDK_AAC_DIR := third_party/fdk-aac
CFLAGS  ?= -O2 -Wall -Wextra -fPIC -Iinclude -Isrc -Isrc/sbc -I$(FDK_AAC_DIR)/include
LDFLAGS ?= $(FDK_AAC_DIR)/lib/libfdk-aac.a -lpthread -lm

BUILD_DIR := build
SRCS      := src/y1_core.c \
             src/y1_display.c \
             src/y1_cpu.c \
             src/y1_input.c \
             src/y1_audio.c \
             src/y1_tuner.c \
             src/y1_usb.c \
             src/y1_ipod_sync.c \
             src/y1_bt.c \
             src/y1_wifi.c \
             src/sbc/sbc.c \
             src/sbc/sbc_primitives.c \
             src/sbc/sbc_primitives_armv6.c

OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
STATIC_LIB := $(BUILD_DIR)/liby1.a
SHARED_LIB := $(BUILD_DIR)/liby1.so

.PHONY: all clean examples

all: $(STATIC_LIB) $(SHARED_LIB)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c include/liby1.h | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(OBJS)
	$(CC) -shared $(CFLAGS) $^ -o $@ $(LDFLAGS)
	$(STRIP) --strip-unneeded $@

examples: $(STATIC_LIB)
	$(CC) $(CFLAGS) examples/hello_y1.c $(STATIC_LIB) -lm -o $(BUILD_DIR)/hello_y1
	$(STRIP) $(BUILD_DIR)/hello_y1

clean:
	rm -rf $(BUILD_DIR)

