#!/usr/bin/env bash

# The -e is not set because we want to get all the mismatch format at once

set -u -o pipefail

# In CI environment, require all formatting tools to be present
# Set CI=true to enforce tool presence (GitHub Actions sets this automatically)
REQUIRE_TOOLS="${CI:-false}"

C_FORMAT_EXIT=0
SH_FORMAT_EXIT=0
PY_FORMAT_EXIT=0

# Use git ls-files to exclude submodules and untracked files
C_SOURCES=()
while IFS= read -r file; do
	[ -n "$file" ] && C_SOURCES+=("$file")
done < <(git ls-files -- 'include/*.h' 'src/*.c' 'src/*.h' 'ports/*.c' 'ports/*.h' 'tests/*.c' 'tests/*.h')

if [ ${#C_SOURCES[@]} -gt 0 ]; then
	if command -v clang-format-20 >/dev/null 2>&1; then
		echo "Checking C files with clang-format-20..."
		clang-format-20 -n --Werror "${C_SOURCES[@]}"
		C_FORMAT_EXIT=$?
	elif command -v clang-format >/dev/null 2>&1; then
		echo "Checking C files with clang-format..."
		clang-format -n --Werror "${C_SOURCES[@]}"
		C_FORMAT_EXIT=$?
	else
		if [ "$REQUIRE_TOOLS" = "true" ]; then
			echo "ERROR: clang-format not found (required in CI)" >&2
			C_FORMAT_EXIT=1
		else
			echo "Skipping C format check: clang-format not found" >&2
		fi
	fi
fi

SH_SOURCES=()
while IFS= read -r file; do
	[ -n "$file" ] && SH_SOURCES+=("$file")
done < <(git ls-files -- '*.sh' '.ci/*.sh' 'scripts/*.sh')

if [ ${#SH_SOURCES[@]} -gt 0 ]; then
	if command -v shfmt >/dev/null 2>&1; then
		echo "Checking shell scripts..."
		MISMATCHED_SH=$(shfmt -l "${SH_SOURCES[@]}")
		if [ -n "$MISMATCHED_SH" ]; then
			echo "The following shell scripts are not formatted correctly:"
			printf '%s\n' "$MISMATCHED_SH"
			shfmt -d "${SH_SOURCES[@]}"
			SH_FORMAT_EXIT=1
		fi
	else
		if [ "$REQUIRE_TOOLS" = "true" ]; then
			echo "ERROR: shfmt not found (required in CI)" >&2
			SH_FORMAT_EXIT=1
		else
			echo "Skipping shell script format check: shfmt not found" >&2
		fi
	fi
fi

PY_SOURCES=()
while IFS= read -r file; do
	[ -n "$file" ] && PY_SOURCES+=("$file")
done < <(git ls-files -- 'scripts/*.py')

if [ ${#PY_SOURCES[@]} -gt 0 ]; then
	if command -v black >/dev/null 2>&1; then
		echo "Checking Python files..."
		black --check --diff "${PY_SOURCES[@]}"
		PY_FORMAT_EXIT=$?
	else
		if [ "$REQUIRE_TOOLS" = "true" ]; then
			echo "ERROR: black not found (required in CI)" >&2
			PY_FORMAT_EXIT=1
		else
			echo "Skipping Python format check: black not found" >&2
		fi
	fi
fi

# Use logical OR to avoid exit code overflow (codes are mod 256)
if [ $C_FORMAT_EXIT -ne 0 ] || [ $SH_FORMAT_EXIT -ne 0 ] || [ $PY_FORMAT_EXIT -ne 0 ]; then
	exit 1
fi
exit 0
