#!/bin/bash
# check_types.sh - our C/C++ code uses src/common/types.h's names (u8..u64,
# i8..i64, usize, isize, f32, f64), never the C/stdint spellings. Run by
# `make test`; prints every offending line and exits 1 if there is one.
#
# Allowed:
#   - casts to (long long) / (unsigned long long) / (unsigned long): they
#     match printf's %lld / %llu / %lu on the car and on the host alike
#   - comment lines, and the words inside string literals
#   - a line marked "types-ok" (a deliberate exception, e.g. hello8's sizeof
#     checks of the C types themselves)
#   - third-party code: src/common/qnx/ (sysroot stand-ins),
#     src/common/mbedtls/, and types.h itself
# sdk/ is not checked: its headers and stubs mirror the unit's own APIs.
set -euo pipefail

cd "$(dirname "$0")/.."
pat='\b(u?int(8|16|32|64)_t|u?intptr_t|size_t|ssize_t|ptrdiff_t|double|float|unsigned char|signed char|long long|unsigned long|unsigned short|short)\b'

# grep exits 1 when nothing matches: that is the good case, hence "|| true".
mapfile -t files < <(find src tests \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) |
  grep -v -E 'src/common/qnx/|src/common/mbedtls/|src/common/types\.h$' | sort)
bad=$(grep -n -E "$pat" "${files[@]}" |
  grep -v 'types-ok' |
  grep -v -E '^[^:]+:[0-9]+:[[:space:]]*(//|/\*|\*)' |
  sed -E -f <(printf '%s\n' \
    's/\((unsigned )?long( long)?\)//g' \
    's/"([^"\\]|\\.)*"/""/g' \
    's#//.*$##' \
    's#/\*.*(\*/|$)##') |
  grep -E "$pat" || true)

if [[ -n "$bad" ]]; then
  echo "check_types: use src/common/types.h's names (docs/native-apps.md §4):"
  echo "$bad"
  exit 1
fi
echo "check_types: ok (${#files[@]} files)"
