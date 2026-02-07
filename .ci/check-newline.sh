#!/usr/bin/env bash

set -e -u -o pipefail

ret=0
show=0

# Check all tracked text files for trailing newlines
# Excludes: externals/ (third-party code with own style), binary files
# Reference: https://medium.com/@alexey.inkin/how-to-force-newline-at-end-of-files-and-why-you-should-do-it-fdf76d1d090e
while IFS= read -rd '' f; do
	# Skip externals directory (third-party code)
	case "$f" in
	externals/*) continue ;;
	esac

	# Skip empty files (e.g., __init__.py markers)
	[ -s "$f" ] || continue

	if file --mime-encoding "$f" | grep -qv binary; then
		tail -c1 <"$f" | read -r _ || show=1
		if [ $show -eq 1 ]; then
			echo "Warning: No newline at end of file $f"
			ret=1
			show=0
		fi
	fi
done < <(git ls-files -z)

exit $ret
