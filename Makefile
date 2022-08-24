# SPDX-License-Identifier: GPL-2.0
#

ifneq ($(CROSS_COMPILE),)
CC = $(CROSS_COMPILE)/bin/aarch64-linux-gnu-gcc
VAR_CFLAGS := -I$(CROSS_COMPILE)/include/tracefs -I$(CROSS_COMPILE)/include/traceevent
VAR_LDLIBS := -L$(CROSS_COMPILE)/lib64 -ltracefs -ltraceevent
else
CC = gcc
VAR_CFLAGS := $(shell pkg-config --cflags libtracefs 2>/dev/null)
VAR_LDLIBS := $(shell pkg-config --libs libtracefs 2>/dev/null)
endif

TARGETS = blk-trace1 blk-trace2

CFLAGS = -static -Wall -Wextra -g -O2 $(VAR_CFLAGS) -pedantic -pthread
LDFLAGS = -lpthread $(VAR_LDLIBS)  -pedantic -pthread -ldl -lm


DEPFLAGS = -Wp,-MMD,$(@D)/.$(@F).d,-MT,$@ 

.PHONY: all
all: $(TARGETS)

.c.o:
ifdef C
	$(check) $<
endif
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

blk-trace1: blk-trace1.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
blk-trace2: blk-trace2.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o  $@

.PHONY: clean
clean:
	$(RM) *.o $(TARGETS) .*.o.d  

prefix ?= /usr/local
sbindir ?= ${prefix}/sbin

install: all
	install -d $(DESTDIR)$(sbindir)
	install -m 755 -p $(TARGETS) $(DESTDIR)$(sbindir)
