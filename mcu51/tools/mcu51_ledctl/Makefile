TARGET ?= mcu51_ledctl
CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

DEVMEM_DIR = ../mcu51_up
SRCS = mcu51_ledctl.c $(DEVMEM_DIR)/devmem.c
OBJS = mcu51_ledctl.o devmem.o

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

mcu51_ledctl.o: mcu51_ledctl.c $(DEVMEM_DIR)/devmem.h
	$(CC) $(CFLAGS) -I$(DEVMEM_DIR) -c -o $@ mcu51_ledctl.c

devmem.o: $(DEVMEM_DIR)/devmem.c $(DEVMEM_DIR)/devmem.h
	$(CC) $(CFLAGS) -I$(DEVMEM_DIR) -c -o $@ $(DEVMEM_DIR)/devmem.c

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)/usr/sbin/mcu51-ledctl
