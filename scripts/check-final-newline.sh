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

set -euo pipefail

readonly DEFAULT_PATHSPEC="."
readonly EVERY_LINE_PATTERN=""

readonly FINAL_BYTE_COUNT=1

readonly FAILURE_EXIT_CODE=1

main() {
  local repository_root
  repository_root="$(git rev-parse --show-toplevel)"
  cd "${repository_root}"

  local offenders
  offenders="$(find_files_without_final_newline "${@:-${DEFAULT_PATHSPEC}}")"
  if [[ -n "${offenders}" ]]; then
    echo "These files must end with a newline!" >&2
    echo "${offenders}" >&2
    exit "${FAILURE_EXIT_CODE}"
  fi

  echo "All text files end with a newline."
}

find_files_without_final_newline() {
  local file
  while IFS= read -r -d '' file; do
    if is_missing_final_newline "${file}"; then
      echo "${file}"
    fi
  done < <(git grep -I --name-only -z -e "${EVERY_LINE_PATTERN}" -- "${@}")
}

is_missing_final_newline() {
  local -r file="${1}"
  if [[ ! -s "${file}" ]]; then
    return 1
  fi

  local final_byte
  final_byte="$(tail -c "${FINAL_BYTE_COUNT}" "${file}")"
  [[ -n "${final_byte}" ]]
}

main "${@}"
