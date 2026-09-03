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
CXXFLAGS_COMMON="-Iinclude -std=c++17 -pthread"
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

# The wrapper spells the core half of the suffix itself rather than wrapping
# _TLSF_ABI, so a core knob added to one and not the other would go unnoticed:
# the wrapper stays self-consistent, links, and silently stops tracking a knob
# that still moves the embedded 'tlsf_t'. Check a core knob on this target too.
check fails "-DTLSF_MAX_POOL_BITS=20" thread "thread: TLSF_MAX_POOL_BITS mismatch"

# TLSF_C11_THREADS does not decide the lock backend on its own. The header
# selects on _TLSF_USE_C11_THREADS, which also needs the compiler to have a
# usable '<threads.h>', so whether the flag changes anything is a property of
# the host. Ask the header which backend it picked rather than inferring it.
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
	printf("%d\n", _TLSF_THREAD_BACKEND);
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

# The two Windows natives are the pair no build system chooses between: the
# header picks from '_WIN32_WINNT' and the compiler version, both of which a
# caller can set per translation unit, while 'SRWLOCK' is one pointer and
# 'CRITICAL_SECTION' is 40 bytes on x86-64. A boolean C11-or-not suffix gave
# both the same name, so a mismatched pair linked and then read 'base' and
# 'capacity' at each other's offsets. Not at a different arena stride: 32 bytes
# fit inside the cache-line padding, so both give the same
# 'sizeof(tlsf_thread_t)', which is also why measuring is no substitute for this
# check. Nothing here can link Windows code, so drive each arm through the
# preprocessor alone against an empty 'windows.h' and require two distinct
# names.
#
# Names, not the '_TLSF_THREAD_BACKEND' ids behind them. The id is the input to
# the paste and the name is what the linker matches on, so comparing ids would
# still pass if the paste stopped consuming the id, which is the one edit that
# would bring the shared-suffix bug back. Ask for the expansion of
# 'tlsf_thread_init' and compare that.
#
# The header reaches SRWLOCK by four routes, and only the '_WIN32_WINNT' one is
# under this script's control. The other three key off the host compiler, so
# they answer the same on both invocations and would collapse the low arm onto
# the high one: undefine what identifies clang and MinGW, or the check reports a
# failure on an MSYS2 host and proves nothing on any host.
mkdir -p "$tmp/winstub"
: >"$tmp/winstub/windows.h"

cat >"$tmp/win_backend.c" <<'EOF'
#include "tlsf_thread.h"
NAME tlsf_thread_init
EOF

# Swallowing the preprocessor's exit status, not its diagnostics: those still
# reach the job log. A directive the header rejects under these defines would
# otherwise take 'set -e' out through the command substitution below, ending the
# script on a bare status with none of the checks reported. A failure here has
# to print its FAIL line like every other check, which is what the empty-value
# default below is for.
win_symbol() {
	# shellcheck disable=SC2086
	$CC -Iinclude -I"$tmp/winstub" -std=gnu11 -D_WIN32 \
		-U__clang__ -U__MINGW32__ -U__MINGW64__ "$@" \
		-E -P "$tmp/win_backend.c" | sed -n 's/^NAME //p' || true
}

srwlock="$(win_symbol -D_WIN32_WINNT=0x0600)"
crsection="$(win_symbol -D_WIN32_WINNT=0x0501)"

# Both names have to be there before comparing them, since a probe that failed
# yields an empty one and empty differs from everything. An unsuffixed name is
# the other way to pass without meaning it: the renaming did not happen at all,
# which is a shared suffix by another route.
if [ -n "$srwlock" ] && [ -n "$crsection" ] &&
	[ "$srwlock" != "tlsf_thread_init" ] &&
	[ "$crsection" != "tlsf_thread_init" ] &&
	[ "$srwlock" != "$crsection" ]; then
	echo "ok: thread: SRWLOCK and CRITICAL_SECTION differ" \
		"($srwlock vs $crsection)"
else
	echo "FAIL: thread: Windows lock backends share a symbol name" \
		"($srwlock vs $crsection)"
	ret=1
fi

# The C++ adapters in include/tlsf_pmr.hpp and include/tlsf_thread_pmr.hpp would
# be a hole in everything above if their classes were left untagged. A
# header-defined class has weak definitions for its vtable and its inline
# virtuals, and those mangled names carry no configuration, so two mismatched
# units emit the same 'do_allocate' twice. A linker that keeps one COMDAT group
# and discards the other discards the discarded copy's reference to the suffixed
# C symbol with it, and the undefined reference that should have failed the link
# is gone. The classes sit in inline namespaces named by the same suffix so that
# cannot happen.
#
# Names, not link behavior, for the reason the Windows arm above gives: whether
# a mismatch reaches the linker is a property of the object format. Mach-O keeps
# the reference and rejects the link with or without the tagging, so a link test
# would pass there while proving nothing. The mangled name differs on every
# host.
CXX="${CXX:-c++}"

cat >"$tmp/pmr.cpp" <<'EOF'
#include "tlsf_pmr.hpp"
#include "tlsf_thread_pmr.hpp"
static tlsf_t core;
static tlsf_thread_t threaded;
tlsf::pmr_resource core_res(core);
tlsf::pmr_thread_resource thread_res(threaded);
EOF

# Print the last probe's compiler output, indented, if there is any. The file is
# absent when the probe never ran, and a redirection from a missing file is a
# shell error that no redirection on the reader can silence.
pmr_log() {
	[ -f "$tmp/pmr.log" ] && sed 's/^/    /' <"$tmp/pmr.log"
	return 0
}

# $1 selects the class, $2 and up are the caller's configuration flags.
#
# The object is named after the flags and reused. Every pmr_check needs the
# unflagged baseline, so compiling on each call rebuilt an unchanged file once
# per check, and the same variant is asked for by two different checks.
#
# Keyed on a checksum, not on the flags with punctuation folded away: that
# folding maps '-DX=8' and '-DX 8' to one name, and reusing the wrong object
# here would answer "same" to a check whose whole purpose is noticing when two
# configurations differ.
pmr_symbol() {
	which="$1"
	shift
	obj="$tmp/pmr$(printf '%s' "$*" | cksum | tr -cd '0-9').o"
	if [ ! -f "$obj" ]; then
		# shellcheck disable=SC2086
		$CXX $CXXFLAGS_COMMON -O0 "$@" -c -o "$obj" \
			"$tmp/pmr.cpp" >"$tmp/pmr.log" 2>&1 || return 1
	fi
	case "$which" in
	core) nm "$obj" | grep do_allocate | grep pmr_resource |
		grep -v pmr_thread_resource ;;
	thread) nm "$obj" | grep do_allocate | grep pmr_thread_resource ;;
	esac | awk '{print $NF}' | head -1
}

# pmr_compiles <flag> -- the adapters must still build under <flag>.
pmr_compiles() {
	# shellcheck disable=SC2086
	if $CXX $CXXFLAGS_COMMON "$1" -fsyntax-only "$tmp/pmr.cpp" \
		>"$tmp/pmr.log" 2>&1; then
		echo "ok: pmr: adapters compile with $1"
	else
		echo "FAIL: pmr: adapters no longer compile with $1"
		pmr_log
		ret=1
	fi
}

# same <core|thread> <expected same|differ> <description> <flags...>
pmr_check() {
	which="$1"
	expect="$2"
	desc="$3"
	shift 3
	base="$(pmr_symbol "$which" || true)"
	other="$(pmr_symbol "$which" "$@" || true)"
	if [ -z "$base" ] || [ -z "$other" ]; then
		echo "FAIL: $desc: no do_allocate symbol found"
		pmr_log
		ret=1
		return
	fi
	if [ "$base" = "$other" ]; then
		got=same
	else
		got=differ
	fi
	if [ "$got" != "$expect" ]; then
		echo "FAIL: $desc: expected $expect, got $got ($base vs $other)"
		ret=1
		return
	fi
	echo "ok: $desc ($got)"
}

# Two probes, not one. The first asks whether this toolchain has what the
# adapters need, C++17 '<memory_resource>', and a no there is a skip. Only then
# is a failure to compile the adapters themselves the regression this section
# exists to catch. Deciding both from a single compile of pmr.cpp would report a
# header that stopped compiling as a note and exit 0, which is the guard passing
# while testing nothing.
cat >"$tmp/pmr_probe.cpp" <<'EOF'
#include <memory_resource>
std::pmr::memory_resource *probe = std::pmr::new_delete_resource();
EOF

# No 'command -v' guard on $CXX: it cannot answer for a value carrying flags,
# which is an ordinary way to pass them, and a no there skipped everything below
# without a word. Let the compile decide, and print why.
# shellcheck disable=SC2086
if ! $CXX -std=c++17 -fsyntax-only "$tmp/pmr_probe.cpp" \
	>"$tmp/pmr.log" 2>&1; then
	echo "note: skipping pmr checks: $CXX has no usable" \
		"C++17 '<memory_resource>'"
	pmr_log
elif ! $CXX $CXXFLAGS_COMMON -fsyntax-only "$tmp/pmr.cpp" \
	>"$tmp/pmr.log" 2>&1; then
	echo "FAIL: pmr: $CXX has what the adapters need" \
		"but cannot compile them"
	pmr_log
	ret=1
else
	# -fno-exceptions is ordinary in embedded C++, and do_is_equal() promises
	# in its comment that it declines to compare pools so the headers stay
	# usable under -fno-rtti. Both are claims about where these headers can be
	# built, and the change most likely to break either is the one a reviewer
	# will propose, so each gets a check rather than a comment.
	pmr_compiles -fno-exceptions
	pmr_compiles -fno-rtti

	pmr_check core differ "pmr: TLSF_MAX_POOL_BITS reaches pmr_resource" \
		-DTLSF_MAX_POOL_BITS=20
	pmr_check thread differ \
		"pmr: TLSF_ARENA_COUNT reaches pmr_thread_resource" \
		-DTLSF_ARENA_COUNT=8

	# The two classes are tagged separately on purpose. Folding them into one
	# namespace would reject a pair of units differing only in a knob that does
	# not move 'tlsf_t', which is a working build turned into a link error.
	pmr_check core same "pmr: a thread knob leaves pmr_resource alone" \
		-DTLSF_ARENA_COUNT=8
fi

exit $ret
