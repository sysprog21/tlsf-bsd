OUT = build

TARGETS = \
	test \
	bench
TARGETS := $(addprefix $(OUT)/,$(TARGETS))

all: $(TARGETS)

test: all
	./build/bench
	./build/bench -s 32
	./build/bench -s 10:12345
	./build/test

CFLAGS += \
  -std=gnu11 -g -O2 \
  -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wconversion -Wc++-compat \
  -DTLSF_ENABLE_ASSERT -DTLSF_ENABLE_CHECK

OBJS = tlsf.o
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(OBJS:%.o=%.o.d)

$(OUT)/test: $(OBJS) test.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/bench: $(OBJS) bench.c
	$(CC) $(CFLAGS) -o $@ -MMD -MF $@.d $^ $(LDFLAGS)

$(OUT)/%.o: %.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ -MMD -MF $@.d $<

check: $(TARGETS)
	MALLOC_CHECK_=3 ./build/test
	MALLOC_CHECK_=3 ./build/bench

clean:
	$(RM) $(TARGETS) $(OBJS) $(deps)

.PHONY: all check clean test

-include $(deps)
