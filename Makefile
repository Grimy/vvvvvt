# st - simple terminal
# See LICENSE file for copyright and license details.

VERSION = 0.7

# includes and libs
INCS = -I. -I/usr/include -I/usr/X11R6/include \
       `pkg-config --cflags fontconfig` \
       `pkg-config --cflags freetype2`
LIBS = -L/usr/lib -lc -L/usr/X11R6/lib -lm -lrt -lX11 -lutil -lXft \
       `pkg-config --libs fontconfig`  \
       `pkg-config --libs freetype2`

# flags
CPPFLAGS = -DVERSION=\"${VERSION}\" -D_XOPEN_SOURCE=600
CFLAGS += -g -std=c99 -pedantic -Wall -Wextra -Wno-sign-compare -Os ${INCS} ${CPPFLAGS}
LDFLAGS += -g ${LIBS}

st: st.c
	@echo CC $@
	@$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

config.h:
	cp config.def.h config.h

${OBJ}: config.h config.mk

.PHONY: all options clean dist install uninstall
