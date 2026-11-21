# Makefile — lokalizator GPS (ATmega64)
# Oryginal skompilowano w BASCOM-AVR; ta rekonstrukcja w C buduje sie avr-gcc.
#
#   make            # kompilacja -> build/tracker.elf + build/tracker.hex
#   make size       # rozmiar sekcji
#   make flash      # wgranie przez avrdude (ustaw PROGRAMMER/PORT)
#   make disasm     # zrzut deasemblera z ELF
#   make clean

MCU        := atmega64
F_CPU      := 8000000UL

CC         := avr-gcc
OBJCOPY    := avr-objcopy
OBJDUMP    := avr-objdump
SIZE       := avr-size

PROGRAMMER ?= usbasp
PORT       ?= usb

SRCDIR     := src
INCDIR     := include
BUILDDIR   := build

SRCS       := $(wildcard $(SRCDIR)/*.c)
OBJS       := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

TARGET     := $(BUILDDIR)/tracker

CFLAGS     := -mmcu=$(MCU) -DF_CPU=$(F_CPU) -I$(INCDIR) \
              -Os -Wall -Wextra -std=gnu99 \
              -ffunction-sections -fdata-sections
LDFLAGS    := -mmcu=$(MCU) -Wl,--gc-sections

.PHONY: all size flash disasm clean

all: $(TARGET).hex size

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(TARGET).elf
	$(SIZE) --format=avr --mcu=$(MCU) $<

disasm: $(TARGET).elf
	$(OBJDUMP) -d $< > $(TARGET).lss

flash: $(TARGET).hex
	avrdude -c $(PROGRAMMER) -p m64 -P $(PORT) -U flash:w:$<:i

clean:
	rm -rf $(BUILDDIR)
