#!/usr/bin/env bash
# Fails if any tracked, non-empty text file does not end with a newline.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

missing=0
while IFS= read -r -d '' file; do
    if [ -s "$file" ] && [ -n "$(tail -c 1 "$file")" ]; then
        echo "missing final newline: $file"
        missing=1
    fi
done < <(git grep -I --name-only -z -e '' -- "${@:-.}")

if [ "$missing" -ne 0 ]; then
    echo "Add a trailing newline to the files above." >&2
    exit 1
fi
echo "All text files end with a newline."
