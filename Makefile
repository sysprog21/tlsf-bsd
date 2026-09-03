OUT = build

.DEFAULT_GOAL := all

FRAMAC ?= frama-c

# Leaf helpers carrying ACSL contracts. No caller of these is in the list yet,
# so every 'requires' here is an assumed hypothesis rather than a discharged
# one: 'make verify' proves the helpers consistent with their own contracts,
# not that the allocator establishes those contracts at the call sites. Extend
# upward (block_split, block_absorb, block_set_free, ...) to close that gap.
#
# Read the goal count for what it is. This list is a subset of the functions
# defined in src/tlsf.c, and tlsf_pool_reset is the only public entry point on
# it. tlsf_malloc, tlsf_free, tlsf_realloc, tlsf_aalloc, tlsf_pool_init,
# tlsf_append_pool, tlsf_usable_size, tlsf_check and tlsf_get_stats are all
# unproved, as are the internal arena helpers they call.
# Several listed helpers also have no postcondition, which means "proved" is
# "cannot fault", not "returns the right answer": injecting a real bug into
# align_offset still yields a fully proved run, while the same injection into
# block_set_prev_free, which does carry 'ensures' clauses, is caught. A green
# run here is a floor, not a certificate.
WP_FUNCTIONS = \
	align_up,align_ptr,block_payload,to_block,block_from_payload, \
	block_is_free,block_is_prev_free,block_can_split,block_can_trim, \
	block_size,block_set_size,block_add_size,block_link,block_absorb_at, \
	block_split_headers,block_set_free_bit, \
	block_set_prev_free,block_set_free_at,free_list_link, \
	free_list_unlink,bin_set_head, \
	bins_reset,adjust_size,block_prev,block_poison_free,check_sentinel, \
	bitmap_ffs,log2floor,block_next,round_block_size,floor_block_size, \
	block_size_mask, \
	align_offset,mapping, \
	tlsf_pool_reset,remove_free_block,insert_free_block

TARGETS = \
	test \
	bench \
	wcet \
	fuzz \
	check_negative
TARGETS := $(addprefix $(OUT)/,$(TARGETS))

THREAD_TARGETS = $(OUT)/test_thread
CPP_TARGETS = $(OUT)/test_cpp

# Named outside the probe below so 'clean' can remove it either way. A tree
# built with a C++17 toolchain and cleaned with an older one would otherwise
# keep a stale binary and its dep file, which is the same trap the 'deps'
# comment further down guards against.
PMR_TARGET = $(OUT)/test_pmr

# Full benchmark with statistical rigor (50 iterations, 5 warmup)
bench: all
	build/bench -s 64 -l 1000000 -i 50 -w 5
	build/bench -s 256 -l 1000000 -i 50 -w 5
	build/bench -s 1024 -l 1000000 -i 50 -w 5
	build/bench -s 64:4096 -l 1000000 -i 50 -w 5

# Quick benchmark for development
bench-quick: all
	build/bench -s 64:4096 -l 100000 -i 10 -w 3

# Clear this to build and test the library the way it ships. It is the only
# way the off-branches of TLSF_ENABLE_ASSERT and TLSF_ENABLE_CHECK ever get
# compiled: 'make check TLSF_DEBUG_FLAGS='.
TLSF_DEBUG_FLAGS ?= -DTLSF_ENABLE_ASSERT -DTLSF_ENABLE_CHECK

# Preprocessor state shared by every translation unit, C and C++ alike.
# Configuration macros change the layout of tlsf_t, and a mismatch between two
# objects in the same binary is not diagnosed, so they must not live in a
# language-specific variable. Anything a caller injects through CPPFLAGS
# reaches both compilers for the same reason.
override CPPFLAGS += -Iinclude $(TLSF_DEBUG_FLAGS)

TLSF_WARNINGS = \
  -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wconversion

override CFLAGS += \
  -std=gnu11 -g -O2 \
  $(TLSF_WARNINGS) -Wc++-compat

override CXXFLAGS += \
  -std=c++11 -g -O2 \
  $(TLSF_WARNINGS) -pedantic-errors

# The public headers stay compilable as C++11 while the adapters need C++17.
# Appending a second -std rather than editing CXXFLAGS keeps that split: the
# later flag wins, so tests/test_cpp.cpp still proves the C++11 baseline and
# only this one target is raised. A caller's own '-std' is overridden here, as
# it already is for test_cpp, because 'override CXXFLAGS +=' appends to the
# caller's value rather than replacing it, leaving that value first on the
# command line. Plain '+=' would append only to an environment value and be
# skipped outright for a command-line one, so a command-line CXXFLAGS would
# build test_cpp with no -std at all and pin nothing.
CXX17FLAGS = $(CXXFLAGS) -std=c++17

# The std::pmr adapters are optional, so their probe uses the same
# preprocessor and C++ flags as their target before adding it to 'check'.
#
# No exceptions are needed to use the adapters: a failed allocation terminates
# when they are disabled, because memory_resource cannot report a null result.
PMR_PROBE := $(shell printf \
	'\043include <memory_resource>\nint main(){}\n' \
	| $(CXX) $(CPPFLAGS) $(CXX17FLAGS) -pthread -fsyntax-only \
		-x c++ - 2>/dev/null && echo yes)
ifeq ($(PMR_PROBE),yes)
CPP_TARGETS += $(PMR_TARGET)
endif

all: $(TARGETS) $(THREAD_TARGETS) $(CPP_TARGETS)

# The header's own knobs are spelled '_TLSF_' as well as 'TLSF_', and either
# prefix can arrive as -U rather than -D, so all four forms have to be caught.
ifneq ($(filter -DTLSF_% -D_TLSF_% -UTLSF_% -U_TLSF_%, \
	$(CFLAGS) $(CXXFLAGS) $(CXX17FLAGS)),)
$(error Pass TLSF configuration macros through CPPFLAGS, not CFLAGS or CXXFLAGS)
endif

OBJS = tlsf.o
OBJS := $(addprefix $(OUT)/,$(OBJS))

THREAD_OBJS = $(OUT)/tlsf_thread.o

# Every rule that passes -MMD -MF must be listed, or 'clean' leaves its dep
# file behind. $(OUT)/test is deliberately absent: its rule emits no dep file.
deps := $(OBJS:%.o=%.o.d) $(THREAD_OBJS:%.o=%.o.d) \
	$(OUT)/bench.d $(OUT)/wcet.d $(OUT)/fuzz.d $(OUT)/check_negative.d \
	$(THREAD_TARGETS:%=%.d) $(CPP_TARGETS:%=%.d) $(PMR_TARGET:%=%.d)

# Make compares timestamps, not command lines, so flipping a variable on the
# command line rebuilds nothing. 'make check TLSF_DEBUG_FLAGS=' on an
# already-built tree would relink against the assertions-enabled objects and
# report a pass for a configuration it never compiled. Stamping the toolchain
# and flags, and depending on the stamp, makes any change rebuild. The stamp is
# rewritten only when the value differs, so a no-op build stays a no-op.
#
# The value covers every variable the compile and link recipes expand;
# -pthread and -lm are literals. One stamp covers all of them, so an
# LDFLAGS-only change also recompiles the two objects. Rebuilding more than
# strictly needed is safe, and a second stamp is not worth the machinery here.
FLAGS_STAMP := $(OUT)/.build-flags
BUILD_FLAGS := $(CC) $(CXX) $(CPPFLAGS) $(CFLAGS) $(CXXFLAGS) $(CXX17FLAGS) \
	$(LDFLAGS)

# Exported rather than pasted into the recipe text. A flag value may contain a
# quote, and interpolating one into a quoted shell word would end the string
# early and kill the recipe on a syntax error. A parameter expansion is not
# rescanned, so the value survives whatever it holds. Not $(file), which does no
# shell quoting at all, because it needs make 4.0 and this tree builds on 3.81.
export BUILD_FLAGS
FORCE:
$(FLAGS_STAMP): FORCE
	@mkdir -p $(OUT)
	@printf '%s\n' "$$BUILD_FLAGS" | cmp -s - $@ || \
		printf '%s\n' "$$BUILD_FLAGS" > $@

# The link rules below spell out their sources instead of using $^. The dep
# files pull headers in as prerequisites, and $(FLAGS_STAMP) is one too, so $^
# would hand both to the compiler as source files.
$(OUT)/test: $(OBJS) tests/test.c $(FLAGS_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(OBJS) tests/test.c $(LDFLAGS)

$(OUT)/bench: $(OBJS) tests/bench.c $(FLAGS_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ -MMD -MF $@.d $(OBJS) tests/bench.c \
		$(LDFLAGS) -lm

$(OUT)/wcet: $(OBJS) tests/wcet.c $(FLAGS_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ -MMD -MF $@.d $(OBJS) tests/wcet.c \
		$(LDFLAGS) -lm

# The replay driver, which every toolchain can build and 'make check' runs.
# 'make fuzz' below builds the same file against libFuzzer instead.
$(OUT)/fuzz: $(OBJS) tests/fuzz.c $(FLAGS_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests -o $@ -MMD -MF $@.d $(OBJS) \
		tests/fuzz.c $(LDFLAGS)

# Deliberate heap corruption, one case per process. See the file header.
$(OUT)/check_negative: $(OBJS) tests/check_negative.c $(FLAGS_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests -o $@ -MMD -MF $@.d $(OBJS) \
		tests/check_negative.c $(LDFLAGS)

# Thread-safe module (requires pthreads)
$(OUT)/tlsf_thread.o: src/tlsf_thread.c include/tlsf_thread.h $(FLAGS_STAMP)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c -o $@ -MMD -MF $@.d $<

$(OUT)/test_thread: $(OBJS) $(THREAD_OBJS) tests/test_thread.c \
		$(FLAGS_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ -MMD -MF $@.d $(OBJS) \
		$(THREAD_OBJS) tests/test_thread.c $(LDFLAGS)

$(OUT)/test_cpp: $(OBJS) $(THREAD_OBJS) tests/test_cpp.cpp $(FLAGS_STAMP)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread -o $@ -MMD -MF $@.d $(OBJS) \
		$(THREAD_OBJS) tests/test_cpp.cpp $(LDFLAGS)

$(PMR_TARGET): $(OBJS) $(THREAD_OBJS) tests/test_pmr.cpp $(FLAGS_STAMP)
	$(CXX) $(CPPFLAGS) $(CXX17FLAGS) -pthread -o $@ -MMD -MF $@.d $(OBJS) \
		$(THREAD_OBJS) tests/test_pmr.cpp $(LDFLAGS)

$(OUT)/%.o: src/%.c $(FLAGS_STAMP)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ -MMD -MF $@.d $<

# Every runner below names $(OUT) rather than a literal 'build'. With the
# literal, 'make OUT=elsewhere check' built into one directory and ran whatever
# an earlier build had left in the other, reporting a pass for binaries it had
# not produced; with no 'build' at all it failed on the first line. The path
# carries a slash either way, so no './' prefix is needed, and dropping it is
# also what lets an absolute $(OUT) work.
check: $(TARGETS) $(THREAD_TARGETS) $(CPP_TARGETS) check-negative
	MALLOC_CHECK_=3 $(OUT)/test
	MALLOC_CHECK_=3 $(OUT)/bench -l 10000 -i 3 -w 1
	MALLOC_CHECK_=3 $(OUT)/bench -s 32 -l 10000 -i 3 -w 1
	MALLOC_CHECK_=3 $(OUT)/bench -s 10:12345 -l 10000 -i 3 -w 1
	$(OUT)/wcet -i 100 -w 10
	MALLOC_CHECK_=3 $(OUT)/fuzz
	$(OUT)/test_thread
	$(OUT)/test_cpp
ifeq ($(PMR_PROBE),yes)
	$(PMR_TARGET)
else
	@echo "test_pmr: skipped ($(CXX) lacks C++17 <memory_resource>)"
endif

# Each case must abort with the CHECK diagnostic it names, not with a segfault
# that looks like one and not with some other check that happens to fire first.
# The driver prints the expected message under -e, so the two halves of a case
# cannot drift apart without this failing.
#
# The trailing separator in the pattern below is what makes the message whole
# rather than a prefix of it. Without it a case naming "prev_free bit mismatch"
# would also accept "sentinel prev_free bit mismatch", which is a different
# check in a different walk.
#
# Two ways this fails open, both closed here: a fixture broken in seed() would
# abort every case alike, so the control run goes first; a build without
# TLSF_ENABLE_CHECK has no rejection to observe, so the driver reports zero
# cases and this skips. The count arrives as an exit status, so a driver that
# dies at startup yields a number past the last case, which the driver itself
# then rejects. Cores are off because the aborts are expected.
check-negative: $(OUT)/check_negative
	@ulimit -c 0; \
	$(OUT)/check_negative; n=$$?; \
	if [ "$$n" -eq 0 ]; then \
		echo "check_negative: skipped (built without TLSF_ENABLE_CHECK)"; \
		exit 0; \
	fi; \
	if ! $(OUT)/check_negative -1 >/dev/null 2>&1; then \
		echo "check_negative: control case failed; harness is broken" >&2; \
		exit 1; \
	fi; \
	i=0; \
	while [ "$$i" -lt "$$n" ]; do \
		if ! want=$$($(OUT)/check_negative -e $$i 2>&1); then \
			echo "check_negative: case $$i has no expected message:" >&2; \
			echo "$$want" >&2; exit 1; \
		fi; \
		if out=$$($(OUT)/check_negative $$i 2>&1); then \
			echo "check_negative: case $$i accepted by tlsf_check()" >&2; \
			exit 1; \
		fi; \
		case "$$out" in \
		*"TLSF CHECK: $$want - "*) ;; \
		*) echo "check_negative: case $$i wanted \"$$want\", got:" >&2; \
			echo "$$out" >&2; exit 1 ;; \
		esac; \
		i=$$((i + 1)); \
	done; \
	echo "check_negative: $$n corruptions, each rejected by tlsf_check()"

# RTE guards are emitted as ACSL asserts, so filtering out @assert would discard
# every runtime-error obligation that -wp-rte just generated. Keep them counted.
#
# Two classes of WP warning are expected and cannot be annotated away:
#   "Skipped RTE guards" for \aligned and \valid_function, which the Typed
#   model does not support, and "Cast with incompatible pointers types" for the
#   char* block arithmetic in block_payload()/to_block() and in the assigns
#   clause of block_poison_free(). The Bytes model models
#   those casts natively and silences the warnings, but it is experimental and
#   leaves two goals unproved, so Typed+nocast stays.
verify:
	@command -v $(FRAMAC) >/dev/null 2>&1 || { \
		echo "verify: $(FRAMAC) not found; install Frama-C or set FRAMAC=" >&2; \
		exit 1; \
	}
	@output="$$($(FRAMAC) -cpp-extra-args='-Iinclude -DTLSF_ENABLE_CHECK' \
		-wp -wp-rte -wp-model Typed+nocast -wp-prover Alt-Ergo -wp-timeout 40 \
		-wp-fct='$(WP_FUNCTIONS)' src/tlsf.c 2>&1)"; \
	status=$$?; \
	printf '%s\n' "$$output"; \
	[ $$status -eq 0 ] || exit $$status; \
	printf '%s\n' "$$output" | awk '\
		/Proved goals:/ { found = 1; if ($$4 != $$6) bad = 1 } \
		END { exit !found || bad }'

# Coverage-guided fuzzing. Needs a clang new enough for -fsanitize=fuzzer;
# tests/fuzz.c carries its own main() for every other toolchain, and 'make
# check' runs that one. FUZZ_RUNS bounds a local run; CI passes a smaller
# budget instead. FUZZ_CORPUS is where libFuzzer keeps what it learns; 'clean'
# removes only the default under $(OUT), never a corpus the caller pointed the
# variable at.
FUZZ_CC ?= clang
FUZZ_RUNS ?= 100000
FUZZ_CORPUS ?= $(OUT)/fuzz-corpus
FUZZ_SANITIZERS ?= fuzzer,address,undefined

fuzz: $(FLAGS_STAMP)
	@mkdir -p $(FUZZ_CORPUS)
	$(FUZZ_CC) $(CPPFLAGS) -DTLSF_FUZZ_NO_MAIN -Itests -std=gnu11 -g -O1 \
		-fsanitize=$(FUZZ_SANITIZERS) -o $(OUT)/fuzz-libfuzzer \
		src/tlsf.c tests/fuzz.c $(LDFLAGS)
	$(OUT)/fuzz-libfuzzer -runs=$(FUZZ_RUNS) $(FUZZ_CORPUS)

# Full WCET measurement (10000 iterations, 1000 warmup)
wcet: all
	$(OUT)/wcet

# Quick WCET check for development
wcet-quick: all
	$(OUT)/wcet -i 1000 -w 100

# WCET with raw output and analysis plots
wcet-plot: all
	@mkdir -p $(OUT)
	$(OUT)/wcet -i 10000 -r $(OUT)/wcet_raw.csv -c > $(OUT)/wcet_summary.csv
	python3 scripts/wcet_plot.py $(OUT)/wcet_raw.csv -o $(OUT)/wcet

clean:
	$(RM) $(TARGETS) $(THREAD_TARGETS) $(CPP_TARGETS) $(PMR_TARGET)
	$(RM) $(OBJS) $(THREAD_OBJS) $(deps) $(FLAGS_STAMP)
	$(RM) $(OUT)/wcet_raw.csv $(OUT)/wcet_summary.csv
	$(RM) $(OUT)/wcet_boxplot.png $(OUT)/wcet_histogram.png
	$(RM) $(OUT)/fuzz-libfuzzer
	$(RM) -r $(OUT)/fuzz-corpus
	$(RM) $(OUT)/*.gcda $(OUT)/*.gcno *.gcov
	$(RM) -r $(OUT)/*.dSYM

.PHONY: all check check-negative clean verify bench bench-quick fuzz wcet \
	wcet-quick wcet-plot FORCE

-include $(deps)
