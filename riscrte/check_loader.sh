#!/usr/bin/env bash
set -euo pipefail
sdk="${1:?usage: check_loader.sh SDK_PATH GAMEBOY_ELF}"
elf="${2:?usage: check_loader.sh SDK_PATH GAMEBOY_ELF}"
validator="$(mktemp)"
trap 'rm -f "$validator"' EXIT
cc -std=c11 -Wall -Wextra -Werror \
  -I"$sdk/test/native_apps/stubs" \
  -I"$sdk/lib/elf_loader/include" \
  "$sdk/lib/elf_loader/src/esp_elf_validate.c" \
  "$sdk/test/native_apps/validate_test.c" \
  -o "$validator"
"$validator" "$elf"
