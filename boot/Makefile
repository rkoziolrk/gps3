# Makefile — bootloader OTA (ATmega64, sekcja BOOT @0xF000)
#
#   make                 # -> boot.hex
#   make fuse            # ustaw fuse BOOTRST+BOOTSZ (RAZ, ISP) — ZWERYFIKUJ!
#   make flash           # wgraj bootloader przez ISP
#
# Patrz docs/OTA.md.

MCU        := atmega64
F_CPU      := 8000000UL
BOOT_START := 0xF000           # 2048 słów BOOT: start sekcji (bajtowo)

CC         := avr-gcc
OBJCOPY    := avr-objcopy
SIZE       := avr-size

PROGRAMMER ?= usbasp
PORT       ?= usb

CFLAGS := -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -Wall -Wextra -std=gnu99 \
          -ffunction-sections -fdata-sections -mcall-prologues -mrelax
LDFLAGS := -mmcu=$(MCU) -Wl,--gc-sections -Wl,--relax \
           -Wl,--section-start=.text=$(BOOT_START)

.PHONY: all fuse flash clean

all: boot.hex

boot.elf: boot.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@
	$(SIZE) --format=avr --mcu=$(MCU) $@

boot.hex: boot.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

# Fuse: BOOTRST=0, BOOTSZ=00 (2048 słów). hfuse=0x98 — ZWERYFIKUJ z datasheet!
fuse:
	avrdude -c $(PROGRAMMER) -p m64 -P $(PORT) -U hfuse:w:0x98:m

flash: boot.hex
	avrdude -c $(PROGRAMMER) -p m64 -P $(PORT) -U flash:w:$<:i

clean:
	rm -f boot.elf boot.hex
