CROSS  ?= riscv64-unknown-elf-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy

CFLAGS  = -march=rv32imc -mabi=ilp32 -Os -ffreestanding -nostdlib -Wall
LDFLAGS = -T linker.ld -nostdlib -Wl,--build-id=none

SRCS = start.S main.c interp.c cc.c fb.c gui.c edit.c sched.c ctx_switch.S
OUT  = bootldr

all: $(OUT).hex

$(OUT).elf: $(SRCS) linker.ld interp.h cc.h fb.h gui.h font5x7.h edit.h sched.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S main.c interp.c cc.c fb.c gui.c edit.c sched.c ctx_switch.S

$(OUT).bin: $(OUT).elf
	$(OBJCOPY) -O binary $< $@

# 8192 words = 32 KB BRAM image. Bootloader at start, rest zero-padded.
$(OUT).hex: $(OUT).bin
	python3 ../tools/bin2hex.py $< $@ 8192

clean:
	rm -f $(OUT).elf $(OUT).bin $(OUT).hex

.PHONY: all clean
