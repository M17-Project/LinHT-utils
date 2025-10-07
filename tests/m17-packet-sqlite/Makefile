optim = -O2 -mcpu=cortex-a55 -mtune=cortex-a55 -ftree-vectorize
warnings = -Wall -Wextra
libs = -lm -lm17 -llinht-ctrl -lsqlite3 -lzmq

all: m17-packet-sqlite.c
	gcc $(optim) $(warnings) m17-packet-sqlite.c -o m17-packet-sqlite $(libs)

clean:
	rm -f m17-packet-sqlite
