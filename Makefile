# comic - minimalistic image viewer
# See LICENSE file for copyright and license details.

include config.mk

SRC = comic.c
OBJ = ${SRC:.c=.o}

all: options comic

options:
	@echo comic build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

config.h:
	@echo creating $@ from config.def.h
	@cp config.def.h $@

comic: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ comic.o ${LDFLAGS}

clean:
	@echo cleaning
	@rm -f comic ${OBJ}

install: all
	@echo installing executables to ${DESTDIR}${PREFIX}/bin
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@cp -f comic comic_dir.sh ${DESTDIR}${PREFIX}/bin
	@chmod 755 ${DESTDIR}${PREFIX}/bin/comic
	@chmod 755 ${DESTDIR}${PREFIX}/bin/comic_dir.sh

uninstall:
	@echo removing executables from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/comic
	@rm -f ${DESTDIR}${PREFIX}/bin/comic_dir.sh

.PHONY: all options clean
