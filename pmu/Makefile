TARGET ?= attiny826
COM_PORT ?= /dev/ttyUSB0
UPDI_BAUD ?= 125k
PYMCUPROG ?= pymcuprog
FUSES_FILE ?= fuses.hex
BUILD_DIR ?= build
HOST_CC ?= cc

CFLAGS ?= -Os -Wall -Wextra -Werror -std=gnu11
MCUFLAGS = -mmcu=$(TARGET) -DF_CPU=1000000UL

# fuses.hex: 8K watchdog locked after reset, BOD 2.6 V, 16 MHz oscillator,
# PA0 UPDI, 64 ms startup. Firmware divides the oscillator by 16.
.PHONY: all clean test program ping read_fuses fuses

all: $(BUILD_DIR)/pmu-revC.hex

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/pmu-revC.elf: main.c pmu.c pmu.h Makefile | $(BUILD_DIR)
	avr-gcc $(CFLAGS) $(MCUFLAGS) main.c pmu.c -o $@
	avr-size $@

$(BUILD_DIR)/pmu-revC.hex: $(BUILD_DIR)/pmu-revC.elf
	avr-objcopy -O ihex $< $@

$(BUILD_DIR)/test-pmu: tests/test_pmu.c pmu.c pmu.h Makefile | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -I. tests/test_pmu.c pmu.c -o $@

test: $(BUILD_DIR)/test-pmu
	$(BUILD_DIR)/test-pmu

clean:
	rm -rf $(BUILD_DIR)

ping:
	$(PYMCUPROG) ping -d $(TARGET) -t uart -u $(COM_PORT) -c $(UPDI_BAUD)

program: $(BUILD_DIR)/pmu-revC.hex
	$(PYMCUPROG) write -f $< -d $(TARGET) -t uart -u $(COM_PORT) -c $(UPDI_BAUD) --erase --verify

read_fuses:
	$(PYMCUPROG) read -m fuses -d $(TARGET) -t uart -u $(COM_PORT) -c $(UPDI_BAUD)

fuses:
	$(PYMCUPROG) write -f $(FUSES_FILE) -d $(TARGET) -t uart -u $(COM_PORT) -c $(UPDI_BAUD)
