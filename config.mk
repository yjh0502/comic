# comic version
VERSION = 0.1

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

# includes and libs
INCS = -I. -I/usr/include -I${X11INC}
LIBS = -L/usr/local/lib -lc -L${X11LIB} -lX11 -ljpeg -larchive

# flags
CPPFLAGS = -DVERSION=\"${VERSION}\" -D_BSD_SOURCE
CFLAGS = -std=c99 -pedantic -Wall -Os ${INCS} ${CPPFLAGS} \
	-Wall -Werror -Wno-deprecated-declarations -g -ggdb

LDFLAGS = -g ${LIBS}

# compiler and linker
CC = cc
