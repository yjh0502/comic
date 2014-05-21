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
	@rm -f comic ${OBJ}

.PHONY: all options clean
