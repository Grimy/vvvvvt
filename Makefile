# st - simple terminal
# See LICENSE file for copyright and license details.

CC = clang
CPPFLAGS = -D_POSIX_C_SOURCE=200809
CFLAGS += -g -std=c99 -Weverything -Werror -O3 -fno-inline ${INCS} ${CPPFLAGS}
CFLAGS += -Wno-sign-conversion -Wno-switch -Wno-gnu-case-range
CFLAGS += `pkg-config --cflags fontconfig`
LDFLAGS = -lutil -lX11 -lXft `pkg-config --libs fontconfig`

st: st.c colors.c config.h Makefile
	@echo CC $@
	@$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

st-fuzz: st.c colors.c config.h Makefile
	@echo CC $@
	afl-clang $(CFLAGS) $(LDFLAGS) $< -o $@

colors.c: colors.pl
	./$^ > $@

fuzz: st-fuzz
	afl-fuzz -iinput -ooutput -m99M ./$^ cat @@

report: st
	time perf record ./st perl -E 'say "\e[1;3;7mOù‽***" x 3e7'
	perf report

.PHONY: report fuzz
