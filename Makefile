CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs)
TARGET  = atic_atac

all: $(TARGET)

$(TARGET): atic_atac.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
