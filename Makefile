OUT = build

TARGETS = \
	test \
	bench \
	wcet
TARGETS := $(addprefix $(OUT)/,$(TARGETS))

all: $(TARGETS)

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
deps := $(OBJS:%.o=%.o.d)

$(OUT)/test: $(OBJS) tests/test.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/bench: $(OBJS) tests/bench.c
	$(CC) $(CFLAGS) -o $@ -MMD -MF $@.d $^ $(LDFLAGS) -lm

$(OUT)/wcet: $(OBJS) tests/wcet.c
	$(CC) $(CFLAGS) -o $@ -MMD -MF $@.d $^ $(LDFLAGS) -lm

$(OUT)/%.o: src/%.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ -MMD -MF $@.d $<

check: $(TARGETS)
	MALLOC_CHECK_=3 ./build/test
	MALLOC_CHECK_=3 ./build/bench -l 10000 -i 3 -w 1
	MALLOC_CHECK_=3 ./build/bench -s 32 -l 10000 -i 3 -w 1
	MALLOC_CHECK_=3 ./build/bench -s 10:12345 -l 10000 -i 3 -w 1
	./build/wcet -i 100 -w 10

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
	$(RM) $(TARGETS) $(OBJS) $(deps)
	$(RM) $(OUT)/wcet_raw.csv $(OUT)/wcet_summary.csv
	$(RM) $(OUT)/wcet_boxplot.png $(OUT)/wcet_histogram.png

.PHONY: all check clean bench bench-quick wcet wcet-quick wcet-plot

-include $(deps)
