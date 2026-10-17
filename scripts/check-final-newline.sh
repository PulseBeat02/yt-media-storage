#!/usr/bin/env bash
# This file is part of yt-media-storage, a tool for encoding media.
# Copyright (C) 2026 Brandon Li <https://brandonli.me/>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
