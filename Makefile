# makefile for aerosmith
#

### PulseAudio
INCPULSE = -I ../pulseaudio/pulseaudio/src
LIBPULSE = -L ../pulseaudio/pulseaudio/src/.libs -lpulse

### SoX
INCSOX = -I ../sox/sox-14.4.2/src
LIBSOX = -L ../sox/sox-14.4.2/src/.libs -lsox

INCS = ${INCPULSE} ${INCSOX}
LIBS = ${LIBPULSE} ${LIBSOX} -lpthread -lrt
CFLAGS += ${INCS} -g -O0
LDFLAGS += ${LIBS}

all:
	gcc ${CFLAGS} -o aerosmith aerosmith.c ${LDFLAGS}
	gcc ${CFLAGS} -o jaded jaded.c ${LDFLAGS}

clean:
	rm -rf aerosmith jaded

.PHONY: all clean
