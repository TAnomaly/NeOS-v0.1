CROSS  ?= riscv64-unknown-elf-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy

CFLAGS  = -march=rv32imc -mabi=ilp32 -Os -ffreestanding -nostdlib -Wall
LDFLAGS = -T linker.ld -nostdlib -Wl,--build-id=none

SRCS = start.S main.c
OUT  = firmware

all: $(OUT).hex

$(OUT).elf: $(SRCS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S main.c

$(OUT).bin: $(OUT).elf
	$(OBJCOPY) -O binary $< $@

$(OUT).hex: $(OUT).bin
	python3 ../tools/bin2hex.py $< $@ 8192

clean:
	rm -f $(OUT).elf $(OUT).bin $(OUT).hex

.PHONY: all clean
