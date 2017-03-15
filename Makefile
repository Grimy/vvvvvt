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
CC = clang
CPPFLAGS = -DVERSION=\"${VERSION}\" -D_XOPEN_SOURCE=600
CFLAGS += -g -std=c99 -Weverything -Werror -O3 -fno-inline ${INCS} ${CPPFLAGS}
CFLAGS += -Wno-sign-conversion -Wno-switch -Wno-gnu-case-range
LDFLAGS += -g ${LIBS}

st: st.c colors.c config.h Makefile
	@echo CC $@
	@$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

st-fuzz: st.c colors.c config.h Makefile
	@echo CC $@
	afl-clang $(CFLAGS) $(LDFLAGS) $< -o $@

colors.c: husl.pl
	./$^ > $@

fuzz: st-fuzz
	afl-fuzz -iinput -ooutput -m99M ./$^ cat @@

report: st
	time perf record ./st perl -E 'say "\e[1;3;7mOù‽" x 4e7'
	perf report

.PHONY: report fuzz
