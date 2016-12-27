# makefile for aerosmith
#

### Cross compiler
CC = ${CROSS_COMPILE}gcc ${DEF_FLAGS64}

### PulseAudio
INCPULSE = -I ../pulseaudio/pulseaudio/src
LIBPULSE = -L ../pulseaudio/pulseaudio/src/.libs -lpulse

### SoX
INCSOX = -I ../sox/sox-14.4.2/src
LIBSOX = -L ../sox/sox-14.4.2/src/.libs -lsox

INCS = ${INCPULSE} ${INCSOX}
LIBS = ${LIBPULSE} ${LIBSOX} -lpthread -lrt -lm
CFLAGS += ${INCS} -Wall -g -O0
LDFLAGS += ${LIBS}

all:
	gcc ${CFLAGS} -o aerosmith aerosmith.c ${LDFLAGS}
	gcc ${CFLAGS} -o jaded jaded.c ${LDFLAGS}
	gcc ${CFLAGS} -o 9lives 9lives.c ${LDFLAGS}
	gcc ${CFLAGS} -o sox-play-only sox-play-only.c ${LDFLAGS}
	gcc ${CFLAGS} -o sox-play-more-slim sox-play-more-slim.c ${LDFLAGS}
	gcc ${CFLAGS} -o sox-play-more2-slim sox-play-more2-slim.c ${LDFLAGS}

clean:
	rm -rf aerosmith jaded 9lives sox-play-only sox-play-more-slim sox-play-more2-slim

.PHONY: all clean
