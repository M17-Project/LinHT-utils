.PHONY: all install clean

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS =
LIBS    = -lm17 -lraylib -lsx1255 -lzmq -llinht-ctrl -lcyaml -lsqlite3 -lm -lpthread -ldl -lrt

TARGET  = gui_test
SRC     = test.c
OBJ     = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install:
	systemctl stop linht-gui-test
	cp $(TARGET) /usr/bin
	systemctl start linht-gui-test

clean:
	rm -f $(TARGET) $(OBJ)
