optim = -O2 -mcpu=cortex-a55 -mtune=cortex-a55 -ftree-vectorize
warnings = -Wall -Wextra
libs = -llinht-ctrl -lzmq

all: ptt-msg.c
	gcc $(optim) $(warnings) ptt-msg.c -o ptt-msg $(libs)

clean:
	rm -f ptt-msg
