#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

# SPDX license header verification script for first-party sources.
# Exits non-zero if any required file lacks the Apache-2.0 SPDX line.

root_dir="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
cd "$root_dir"

status=0
while IFS= read -r f; do
	base="$(basename "$f")"
	if [[ "$base" =~ ^game\.pb\.(cc|h)$ ]]; then
		continue
	fi
	if ! grep -E -q 'SPDX-License-Identifier:\s*Apache-2.0' "$f"; then
		echo "Missing SPDX header: $f" >&2
		status=1
	fi
done < <(git ls-files '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hpp' '*.proto' | grep -v '^third_party/')

if [[ $status -ne 0 ]]; then
	echo "SPDX check failed" >&2
	exit $status
fi
echo "SPDX check passed: all first-party source files contain Apache-2.0 header."
