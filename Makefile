VERSION = 0.1
PREFIX  = /usr/local

PKGS = x11 xinerama xft

CC = cc
CFLAGS  = -std=c99 -Wall -Wextra -Os -D_DEFAULT_SOURCE -DVERSION=\"${VERSION}\" `pkg-config --cflags ${PKGS}`
LDFLAGS = `pkg-config --libs ${PKGS}`

SRC = smawm.c
OBJ = ${SRC:.c=.o}

all: smawm

config.h:
	cp config.def.h $@

${OBJ}: config.h smawm.h

.c.o:
	${CC} -c ${CFLAGS} $<

smawm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f smawm ${OBJ}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f smawm ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/smawm

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/smawm

.PHONY: all clean install uninstall
