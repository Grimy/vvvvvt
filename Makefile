# st - simple terminal
# See LICENSE file for copyright and license details.

CC = clang
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809 -Weverything -Werror
CFLAGS += -Wno-sign-conversion -Wno-switch -Wno-gnu-case-range
CFLAGS += -g -O3 -fno-inline -fno-omit-frame-pointer -fsanitize=address,undefined
CFLAGS += -lutil -lX11 -lXft `pkg-config --cflags --libs fontconfig`

st: st.c colors.c config.h Makefile
	@echo CC $@
	@clang $(CFLAGS) $< -o $@

st-fuzz: st.c colors.c config.h Makefile
	@echo CC $@
	@afl-clang $(CFLAGS) $< -o $@

colors.c: colors.pl
	./$^ > $@

fuzz: st-fuzz
	afl-fuzz -iinput -ooutput -m99M ./$^ cat @@

report: st
	time perf record ./st perl -E 'say "\e[1;3;7mOù‽***" x 3e7'
	perf report

.PHONY: report fuzz
