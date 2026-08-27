#!/usr/bin/env bash
# The layout of 'tlsf_t' and 'tlsf_thread_t' moves with the configuration
# macros, and callers allocate both. Mismatched translation units used to link
# cleanly and then write past the end of the caller's object. The public symbol
# names now carry the layout knobs, so a mismatch is an undefined reference.
#
# Checked both ways. A mismatched pair must fail to link, which is the half that
# catches a regression reintroducing the corruption. A matched pair must still
# link and run, which is the half that catches a guard so aggressive it breaks
# ordinary builds. Only the caller's flags vary, so the library is built once.
set -e -u -o pipefail

CC="${CC:-cc}"
CFLAGS_COMMON="-Iinclude -std=gnu11 -O1 -pthread"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/core.c" <<'EOF'
#include "tlsf.h"
static char pool[1 << 16];
int main(void)
{
	tlsf_t t;
	return tlsf_pool_init(&t, pool, sizeof(pool)) && tlsf_malloc(&t, 64) ? 0 : 1;
}
EOF

cat >"$tmp/thread.c" <<'EOF'
#include "tlsf_thread.h"
static char mem[1 << 20];
int main(void)
{
	tlsf_thread_t ts;
	return tlsf_thread_init(&ts, mem, sizeof(mem)) ? 0 : 1;
}
EOF

# shellcheck disable=SC2086
$CC $CFLAGS_COMMON -c -o "$tmp/tlsf.o" src/tlsf.c
# shellcheck disable=SC2086
$CC $CFLAGS_COMMON -c -o "$tmp/tlsf_thread.o" src/tlsf_thread.c

ret=0

# check <links|fails> <caller flags> <core|thread> <description>
check() {
	expect="$1"
	flags="$2"
	case "$3" in
	core) src="$tmp/core.c" objs="$tmp/tlsf.o" ;;
	thread) src="$tmp/thread.c" objs="$tmp/tlsf.o $tmp/tlsf_thread.o" ;;
	esac
	desc="$4"

	# shellcheck disable=SC2086
	$CC $CFLAGS_COMMON $flags -c -o "$tmp/caller.o" "$src"
	# shellcheck disable=SC2086
	if $CC -pthread -o "$tmp/out" "$tmp/caller.o" $objs \
		>"$tmp/link.log" 2>&1; then
		got=links
	else
		got=fails
	fi

	if [ "$got" != "$expect" ]; then
		echo "FAIL: $desc: expected link to $expect, got $got"
		sed 's/^/    /' <"$tmp/link.log"
		ret=1
		return
	fi

	# A matched build must also work, not merely resolve its symbols.
	if [ "$expect" = links ] && ! "$tmp/out"; then
		echo "FAIL: $desc: linked but exited non-zero"
		ret=1
		return
	fi

	echo "ok: $desc ($got)"
}

check links "" core "core: matched configuration"
check fails "-DTLSF_MAX_POOL_BITS=20" core "core: TLSF_MAX_POOL_BITS mismatch"

# This keeps '_TLSF_FL_MAX' equal on both sides, so only the size-width suffix
# can reject the layout mismatch.
# shellcheck disable=SC2086
$CC $CFLAGS_COMMON -D_TLSF_SIZE_WIDTH=32 -DTLSF_MAX_POOL_BITS=20 -c \
	-o "$tmp/tlsf-w32-fl20.o" src/tlsf.c
# shellcheck disable=SC2086
$CC $CFLAGS_COMMON -DTLSF_MAX_POOL_BITS=20 -c -o "$tmp/caller.o" "$tmp/core.c"
if $CC -pthread -o "$tmp/out" "$tmp/caller.o" "$tmp/tlsf-w32-fl20.o" \
	>"$tmp/link.log" 2>&1; then
	echo "FAIL: core: _TLSF_SIZE_WIDTH mismatch: expected link to fail, got links"
	ret=1
else
	echo "ok: core: _TLSF_SIZE_WIDTH mismatch (fails)"
fi

check links "" thread "thread: matched configuration"
check fails "-DTLSF_ARENA_COUNT=8" thread "thread: TLSF_ARENA_COUNT mismatch"
check fails "-DTLSF_CACHELINE_SIZE=128" thread "thread: TLSF_CACHELINE_SIZE mismatch"

# TLSF_C11_THREADS does not decide the lock backend on its own. The header
# selects on USE_C11_THREADS, which needs the compiler to actually have C11
# threads and is also set on Windows without the flag, so whether the flag
# changes anything is a property of the host. Ask the header which backend it
# picked rather than inferring it.
#
# Do not infer it from sizeof either. On glibc, mtx_t and pthread_mutex_t are
# the same size, so the wrapper measures identical while the lock operations
# compiled into it differ, and mixing those two builds is exactly what this
# guard must reject.
cat >"$tmp/backend.c" <<'EOF'
#include <stdio.h>
#include "tlsf_thread.h"
int main(void)
{
	printf("%d\n", _TLSF_THREAD_C11_THREADS);
	return 0;
}
EOF
# shellcheck disable=SC2086
$CC $CFLAGS_COMMON -o "$tmp/backend_plain" "$tmp/backend.c"
# shellcheck disable=SC2086
$CC $CFLAGS_COMMON -DTLSF_C11_THREADS -o "$tmp/backend_c11" "$tmp/backend.c"
plain="$("$tmp/backend_plain")"
c11="$("$tmp/backend_c11")"

if [ "$plain" = "$c11" ]; then
	echo "note: TLSF_C11_THREADS selects the same lock backend here" \
		"(both $plain), so the two builds must stay compatible"
	check links "-DTLSF_C11_THREADS" thread \
		"thread: TLSF_C11_THREADS with the same backend"
else
	check fails "-DTLSF_C11_THREADS" thread \
		"thread: TLSF_C11_THREADS mismatch"
fi

exit $ret
