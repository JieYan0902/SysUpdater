.PHONY: all clean

CC := /usr/local/arm/arm-2009q3/bin/arm-none-linux-gnueabi-gcc
DIR := $(shell pwd)

OBJS := $(patsubst %.c,%.o,$(wildcard *.c))
SRC := $(patsubst %.o, %.c,$(OBJS))
CFLAGS := -O2 -I./include
LDFLAGS := -lpthread -lm -lrt

all : updaterd

updaterd : $(OBJS)
	$(CC) -o updaterd -O2 $(LDFLAGS) $^
%.o : %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf *.o;
	rm -rf updaterd;

