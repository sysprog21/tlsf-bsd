OUT = build

FRAMAC ?= frama-c

# Leaf helpers carrying ACSL contracts. No caller of these is in the list yet,
# so every 'requires' here is an assumed hypothesis rather than a discharged
# one: 'make verify' proves the helpers consistent with their own contracts,
# not that the allocator establishes those contracts at the call sites. Extend
# upward (block_split, block_absorb, block_set_free, ...) to close that gap.
WP_FUNCTIONS = \
	align_up,align_ptr,block_payload,to_block,block_from_payload, \
	block_is_free,block_is_prev_free,block_can_split,block_can_trim, \
	block_size,block_set_size,block_add_size,block_link,block_absorb_at, \
	block_split_headers,block_set_free_bit, \
	block_set_prev_free,block_set_free_at,free_list_link, \
	free_list_unlink,bin_set_head, \
	bins_reset,adjust_size,block_prev,block_poison_free,check_sentinel, \
	bitmap_ffs,log2floor,block_next,round_block_size,align_offset,mapping

TARGETS = \
	test \
	bench \
	wcet
TARGETS := $(addprefix $(OUT)/,$(TARGETS))

THREAD_TARGETS = $(OUT)/test_thread

all: $(TARGETS) $(THREAD_TARGETS)

# Full benchmark with statistical rigor (50 iterations, 5 warmup)
bench: all
	build/bench -s 64 -l 1000000 -i 50 -w 5
	build/bench -s 256 -l 1000000 -i 50 -w 5
	build/bench -s 1024 -l 1000000 -i 50 -w 5
	build/bench -s 64:4096 -l 1000000 -i 50 -w 5

# Quick benchmark for development
bench-quick: all
	build/bench -s 64:4096 -l 100000 -i 10 -w 3

CFLAGS += \
  -Iinclude \
  -std=gnu11 -g -O2 \
  -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wconversion -Wc++-compat \
  -DTLSF_ENABLE_ASSERT -DTLSF_ENABLE_CHECK

OBJS = tlsf.o
OBJS := $(addprefix $(OUT)/,$(OBJS))

THREAD_OBJS = $(OUT)/tlsf_thread.o

# Every rule that passes -MMD -MF must be listed, or 'clean' leaves its dep
# file behind. $(OUT)/test is deliberately absent: its rule emits no dep file.
deps := $(OBJS:%.o=%.o.d) $(THREAD_OBJS:%.o=%.o.d) \
	$(OUT)/bench.d $(OUT)/wcet.d $(THREAD_TARGETS:%=%.d)

$(OUT)/test: $(OBJS) tests/test.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/bench: $(OBJS) tests/bench.c
	$(CC) $(CFLAGS) -o $@ -MMD -MF $@.d $^ $(LDFLAGS) -lm

$(OUT)/wcet: $(OBJS) tests/wcet.c
	$(CC) $(CFLAGS) -o $@ -MMD -MF $@.d $^ $(LDFLAGS) -lm

# Thread-safe module (requires pthreads)
$(OUT)/tlsf_thread.o: src/tlsf_thread.c include/tlsf_thread.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -pthread -c -o $@ -MMD -MF $@.d $<

$(OUT)/test_thread: $(OBJS) $(THREAD_OBJS) tests/test_thread.c
	$(CC) $(CFLAGS) -pthread -o $@ -MMD -MF $@.d $^ $(LDFLAGS)

$(OUT)/%.o: src/%.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ -MMD -MF $@.d $<

check: $(TARGETS) $(THREAD_TARGETS)
	MALLOC_CHECK_=3 ./build/test
	MALLOC_CHECK_=3 ./build/bench -l 10000 -i 3 -w 1
	MALLOC_CHECK_=3 ./build/bench -s 32 -l 10000 -i 3 -w 1
	MALLOC_CHECK_=3 ./build/bench -s 10:12345 -l 10000 -i 3 -w 1
	./build/wcet -i 100 -w 10
	./build/test_thread

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

# Full WCET measurement (10000 iterations, 1000 warmup)
wcet: all
	./build/wcet

# Quick WCET check for development
wcet-quick: all
	./build/wcet -i 1000 -w 100

# WCET with raw output and analysis plots
wcet-plot: all
	@mkdir -p $(OUT)
	./build/wcet -i 10000 -r $(OUT)/wcet_raw.csv -c > $(OUT)/wcet_summary.csv
	python3 scripts/wcet_plot.py $(OUT)/wcet_raw.csv -o $(OUT)/wcet

clean:
	$(RM) $(TARGETS) $(THREAD_TARGETS) $(OBJS) $(THREAD_OBJS) $(deps)
	$(RM) $(OUT)/wcet_raw.csv $(OUT)/wcet_summary.csv
	$(RM) $(OUT)/wcet_boxplot.png $(OUT)/wcet_histogram.png
	$(RM) -r $(OUT)/*.dSYM

.PHONY: all check clean verify bench bench-quick wcet wcet-quick wcet-plot

-include $(deps)
