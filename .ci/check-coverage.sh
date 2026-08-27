#!/usr/bin/env bash

# Line coverage floor for the core allocator.
#
# The suite is what stands between this allocator and silent heap corruption,
# and nothing used to measure how much of it the suite reached. The first run
# of this script found eight unreached lines, every one of them a rejection
# path that no randomized test can stumble into.
#
# The three correctness binaries run below all link the same build/tlsf.o and
# gcov accumulates counts across runs, so the figure is their union rather than
# whatever one of them happens to touch. bench and wcet are left out: they
# exercise paths the three already cover, and they cost minutes.
#
# Assertions are compiled out for the measurement, heap checking left on. With
# TLSF_ENABLE_ASSERT the ASSERT() lines inside the always-inline helpers carry
# the call to __assert_fail, a basic block a passing run must never enter, so
# gcc reports eight of them as unreached and the figure punishes the suite for
# not failing. gcc and clang also disagree about which line to charge them to,
# which made a floor calibrated on one toolchain fail on the other. Removing
# them leaves every remaining unreached line a real gap.
#
# The floor is a ratchet. Raise it when coverage rises; do not lower it to make
# a change fit, because that is exactly the change worth looking at. What stays
# unreached is the configured-ceiling rejections, in arena_grow(), in the append
# path and in tlsf_pool_init(), which only a reduced TLSF_MAX_POOL_BITS can
# reach and the configuration jobs in CI do cover, plus the guard in
# tlsf_get_stats() against an allocator that claims bytes with no arena, which
# no legal call sequence produces. How many lines gcov charges those to depends
# on the toolchain: one on gcc, four on clang.
set -e -u -o pipefail

FLOOR="${COVERAGE_FLOOR:-99}"
GCOV="${GCOV:-gcov}"

make clean >/dev/null
CFLAGS="--coverage" LDFLAGS="--coverage" \
	make all TLSF_DEBUG_FLAGS=-DTLSF_ENABLE_CHECK

./build/test >/dev/null
./build/test_thread >/dev/null
./build/fuzz >/dev/null

report="$(${GCOV} -b -o build src/tlsf.c)"
printf '%s\n' "$report"

pct="$(printf '%s\n' "$report" |
	awk -F'[:%]' '/Lines executed:/ { print $2; exit }')"
if [ -z "$pct" ]; then
	echo "check-coverage: no line-coverage figure from '$GCOV'" >&2
	exit 1
fi

echo "unreached lines in src/tlsf.c:"
grep '#####' tlsf.c.gcov | sed 's/^/  /' || echo "  none"

awk -v got="$pct" -v floor="$FLOOR" 'BEGIN {
	msg = sprintf("check-coverage: %s%% of lines in src/tlsf.c, floor %s%%",
	              got, floor)
	if (got + 0 < floor + 0) {
		print msg > "/dev/stderr"
		exit 1
	}
	print msg
}'
