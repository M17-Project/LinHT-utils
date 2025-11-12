com_port = com8
target = attiny826

all:
	avr-gcc -O2 -Wall -Wextra -mmcu=${target} main.c -o out.elf
	avr-objcopy -O ihex out.elf out.hex

clean:
	rm *.elf *.hex

program:
	pymcuprog write -f out.hex -d ${target} -t uart -u ${com_port} --erase --verify
