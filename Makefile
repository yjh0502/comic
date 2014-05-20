# comic - simple browser
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
	@rm -f comic ${OBJ} comic-${VERSION}.tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p comic-${VERSION}
	@cp -R LICENSE Makefile config.mk config.def.h README \
		comic-open.sh arg.h TODO.md comic.png \
		comic.1 ${SRC} comic-${VERSION}
	@tar -cf comic-${VERSION}.tar comic-${VERSION}
	@gzip comic-${VERSION}.tar
	@rm -rf comic-${VERSION}

install: all
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@cp -f comic ${DESTDIR}${PREFIX}/bin
	@chmod 755 ${DESTDIR}${PREFIX}/bin/comic
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@sed "s/VERSION/${VERSION}/g" < comic.1 > ${DESTDIR}${MANPREFIX}/man1/comic.1
	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/comic.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/comic
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/comic.1

.PHONY: all options clean dist install uninstall
