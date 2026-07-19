# Cross-compile with SDK toolchain, e.g.:
#   make CROSS_COMPILE=aarch64-linux-gnu-
# Or native for host tests (needs /dev/mem on target only).

TARGET ?= mcu51_up
CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

SRCS = mcu51_up.c devmem.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)/usr/sbin/mcu51-up
