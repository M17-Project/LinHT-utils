TARGET ?= attiny826
COM_PORT ?= /dev/ttyUSB0
UPDI_BAUD ?= 125k
PYMCUPROG ?= pymcuprog
FUSES_FILE ?= fuses.hex

CFLAGS ?= -O2 -Wall -Wextra -std=gnu11 -mmcu=$(TARGET)

# Fuse values are defined in $(FUSES_FILE) and written in one command.
# Current profile:
# - WDTCFG: watchdog ON after reset (8.2s)
# - BODCFG: 2.6V
# - OSCCFG: 16MHz oscillator (FW uses /16 prescaler => 1MHz CPU clock)
# - SYSCFG0/SYSCFG1: UPDI on PA0, startup 64ms

.PHONY: all clean program ping read_fuses fuses

all: out.hex

out.elf: main.c
	avr-gcc $(CFLAGS) $< -o $@

out.hex: out.elf
	avr-objcopy -O ihex $< $@

clean:
	rm -f out.elf out.hex

ping:
	$(PYMCUPROG) ping -d $(TARGET) -t uart -u $(COM_PORT) -c $(UPDI_BAUD)

program: out.hex
	$(PYMCUPROG) write -f out.hex -d $(TARGET) -t uart -u $(COM_PORT) -c $(UPDI_BAUD) --erase --verify

read_fuses:
	$(PYMCUPROG) read -m fuses -d $(TARGET) -t uart -u $(COM_PORT) -c $(UPDI_BAUD)

fuses:
	$(PYMCUPROG) write -f $(FUSES_FILE) -d $(TARGET) -t uart -u $(COM_PORT) -c $(UPDI_BAUD)
