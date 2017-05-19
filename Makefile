CC = clang
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809 -Weverything -Werror
CFLAGS += -Wno-gnu-statement-expression -Wno-gnu-case-range
CFLAGS += -Wno-sign-conversion -Wno-multichar
CFLAGS += -g -O3 -fno-omit-frame-pointer -fstrict-aliasing -fstrict-overflow
CFLAGS += -lutil -lX11 -lXft `pkg-config --cflags --libs fontconfig`
CFLAGS += -fsanitize=address,undefined

vvvvvt: vvvvvt.c Makefile
	@echo CC $@
	@clang $(CFLAGS) $< -o $@

vvvvvt-fuzz: vvvvvt.c Makefile
	@echo CC $@
	@afl-clang-fast $(CFLAGS) -DHEADLESS -Wno-unused-function $< -o $@

fuzz: vvvvvt-fuzz
	mkdir -p fuzz-tests
	for file in tests/*; do sed -r 's/^[ -~]+/^/; /^\^?$$/d; s/ +\|//' $$file >"fuzz-$$file"; done
	afl-fuzz -ifuzz-tests -ooutput -mnone ./$^

report: vvvvvt
	time perf record ./$^ ./bench.sh
	perf report --comms=$^

.PHONY: report fuzz
