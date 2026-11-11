# Builds the patched game tree + port layer. Run from build/game:
#   make -C build/game -f ../../game.mk PLATFORM=headless|3ds
PORT := ../..
PRET_TOOLS ?= $(PORT)/../pokepinballrs/tools
PREPROC := $(PRET_TOOLS)/preproc/preproc
PLATFORM ?= headless
OBJ := $(PORT)/build/obj-$(PLATFORM)
BUILD_ID ?= $(shell git -C $(PORT) rev-parse --short HEAD 2>/dev/null || echo dev)

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -mtp=soft -marm
# The game's data tables were laid out by a compiler that pads every struct
# to 4 bytes and uses int-sized enums; match that so the tables still line up.
GAMEABI := -mstructure-size-boundary=32 -fno-short-enums -funsigned-char
GAMECFLAGS := $(ARCH) $(GAMEABI) -O2 -g -std=gnu89 -fcommon -fno-strict-aliasing -fwrapv \
	-fno-delete-null-pointer-checks -fno-toplevel-reorder -w -Wno-error \
	-fpermissive-compat-dummy
GAMECFLAGS := $(filter-out -fpermissive-compat-dummy,$(GAMECFLAGS))
GAMECFLAGS += -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-int -Wno-return-mismatch
PORTCFLAGS := $(ARCH) -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I$(PORT)/port/include -DPORT_BUILD_ID='"$(BUILD_ID)"'

ifeq ($(PLATFORM),3ds)
CC := arm-none-eabi-gcc
PLAT_SRCS := $(PORT)/platform/ctr.c
PORTCFLAGS += -D__3DS__ -I$(DEVKITPRO)/libctru/include
LDFLAGS := -specs=3dsx.specs $(ARCH) -L$(DEVKITPRO)/libctru/lib -Wl,--no-enum-size-warning
LIBS := -lctru -lm
TARGET := $(PORT)/build/pinball3ds.elf
else
CC := arm-linux-gnueabihf-gcc
PLAT_SRCS := $(PORT)/platform/headless.c
LDFLAGS := -static $(ARCH) -Wl,-z,noexecstack
LIBS := -lm -lpthread
TARGET := $(PORT)/build/pinball_headless
ARCH := $(filter-out -mtp=soft,$(ARCH))
endif

GAME_EXCLUDE := src/agb_sram.c
GAME_SRCS := $(filter-out $(GAME_EXCLUDE),$(wildcard src/*.c))
DATA_SRCS := $(wildcard data/*.s)
PORT_SRCS := $(wildcard $(PORT)/port/src/*.c)

GAME_OBJS := $(patsubst src/%.c,$(OBJ)/game/%.o,$(GAME_SRCS)) $(OBJ)/game/m4a_1.o
DATA_OBJS := $(patsubst data/%.s,$(OBJ)/data/%.o,$(DATA_SRCS)) $(OBJ)/data/port_ram.o
PORT_OBJS := $(patsubst $(PORT)/port/src/%.c,$(OBJ)/port/%.o,$(PORT_SRCS)) \
             $(patsubst $(PORT)/platform/%.c,$(OBJ)/platform/%.o,$(PLAT_SRCS))

all: $(TARGET)

$(shell mkdir -p $(OBJ)/game $(OBJ)/data $(OBJ)/port $(OBJ)/platform)

$(OBJ)/game/%.o: src/%.c
	$(CC) -E -iquote include $(GAMECFLAGS) $< -o $(OBJ)/game/$*.i
	$(PREPROC) $(OBJ)/game/$*.i charmap.txt > $(OBJ)/game/$*.pp.i
	$(CC) $(GAMECFLAGS) -c $(OBJ)/game/$*.pp.i -o $@

$(OBJ)/game/m4a_1.o: src/m4a_1.s
	$(CC) $(ARCH) -mthumb-interwork -c $< -o $@

$(OBJ)/data/%.o: data/%.s
	$(PREPROC) $< charmap.txt | $(CC) -E -P -x c -DPORT_ASM -I include - | $(CC) $(ARCH) -x assembler -c - -o $@

$(OBJ)/data/port_ram.o: port_ram.s
	$(CC) $(ARCH) -c $< -o $@

$(OBJ)/port/%.o: $(PORT)/port/src/%.c $(PORT)/port/include/port.h
	$(CC) $(PORTCFLAGS) -c $< -o $@

$(OBJ)/platform/%.o: $(PORT)/platform/%.c $(PORT)/port/include/port.h
	$(CC) $(PORTCFLAGS) -c $< -o $@

$(TARGET): $(GAME_OBJS) $(DATA_OBJS) $(PORT_OBJS)
	$(CC) $(LDFLAGS) -Wl,-Map,$(TARGET).map $^ $(LIBS) -o $@

.PHONY: all
