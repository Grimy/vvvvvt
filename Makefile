CC = clang
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809 -Weverything -Werror
CFLAGS += -Wno-sign-conversion -Wno-switch -Wno-gnu-case-range
CFLAGS += -g -O3 -fno-inline -fno-omit-frame-pointer -fsanitize=address,undefined
CFLAGS += -lutil -lX11 -lXft `pkg-config --cflags --libs fontconfig`

vvvvvt: vvvvvt.c Makefile
	@echo CC $@
	@clang $(CFLAGS) $< -o $@

vvvvvt-fuzz: vvvvvt.c Makefile
	@echo CC $@
	@afl-clang $(CFLAGS) $< -o $@

fuzz: vvvvvt-fuzz
	afl-fuzz -iinput -ooutput -m99M ./$^ cat @@

report: vvvvvt
	time perf record ./vvvvvt perl -E 'say "\e[1;3;7mOù‽***" x 3e7'
	perf report

.PHONY: report fuzz
