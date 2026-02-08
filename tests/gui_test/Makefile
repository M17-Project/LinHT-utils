.PHONY: all clean

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS =
LIBS    = -lraylib -lsx1255 -lzmq -llinht-ctrl -lcyaml -lsqlite3 -lm -lpthread -ldl -lrt

TARGET  = gui_test
SRC     = test.c
OBJ     = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)
