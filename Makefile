CC = clang
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809 -Weverything -Werror
CFLAGS += -Wno-sign-conversion -Wno-gnu-case-range -Wno-multichar
CFLAGS += -g -O3 -fno-omit-frame-pointer -fstrict-aliasing -fstrict-overflow
CFLAGS += -lutil -lX11 -lXft `pkg-config --cflags --libs fontconfig`

vvvvvt: vvvvvt.c Makefile
	@echo CC $@
	@clang $(CFLAGS) -fsanitize=address,undefined $< -o $@

vvvvvt-fuzz: vvvvvt.c Makefile
	@echo CC $@
	@afl-clang $(CFLAGS) -DHEADLESS -Wno-unused-function $< -o $@

fuzz: vvvvvt-fuzz
	afl-fuzz -itests -ooutput -m99M ./$^ cat @@

report: vvvvvt
	time perf record ./$^ ./bench.sh
	perf report --comms=$^

.PHONY: report fuzz
