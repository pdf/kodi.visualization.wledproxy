#!/bin/sh
# Post-build checks for the add-on library.
#
#   check_elf.sh <library> [expected-machine]
#
# - only ADDON_Create, ADDON_GetTypeVersion and ADDON_GetTypeMinVersion are
#   exported;
# - the newest GLIBC_ and GLIBCXX_ symbol versions required are at or below
#   what the box provides through the build container (2.41 and 3.4.33);
# - the ELF class and machine match (default: ELF64, AArch64).
set -eu

lib=$1
machine=${2:-AArch64}
max_glibc=2.41
max_glibcxx=3.4.33
READELF=${READELF:-readelf}

fail() {
  echo "check_elf: $*" >&2
  exit 1
}

[ -f "$lib" ] || fail "no such file: $lib"

header=$($READELF -h "$lib")
echo "$header" | grep -q 'Class:[[:space:]]*ELF64' || fail "not ELF64"
echo "$header" | grep -q "Machine:.*$machine" || fail "machine is not $machine"

# Defined dynamic symbols (Ndx not UND, section symbols skipped), names
# without version suffix.
exports=$($READELF --dyn-syms -W "$lib" |
  awk '$1 ~ /^[0-9]+:$/ && $4 != "SECTION" && $7 != "UND" && $8 != "" { sub(/@.*/, "", $8); print $8 }' |
  sort -u)
expected=$(printf '%s\n' ADDON_Create ADDON_GetTypeMinVersion ADDON_GetTypeVersion)
if [ "$exports" != "$expected" ]; then
  echo "exported:" >&2
  echo "$exports" >&2
  fail "unexpected exported symbols"
fi

# Highest version needed for a prefix, or empty.
highest() {
  $READELF -V -W "$lib" |
    grep -o "Name: $1[0-9.]*" |
    sed "s/Name: $1//" |
    sort -t. -k1,1n -k2,2n -k3,3n |
    tail -n 1
}

# version_le a b: a <= b
version_le() {
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -n 1)" = "$1" ]
}

glibc=$(highest GLIBC_)
glibcxx=$(highest GLIBCXX_)
[ -z "$glibc" ] || version_le "$glibc" "$max_glibc" || fail "needs GLIBC_$glibc > $max_glibc"
[ -z "$glibcxx" ] || version_le "$glibcxx" "$max_glibcxx" ||
  fail "needs GLIBCXX_$glibcxx > $max_glibcxx"

echo "check_elf: OK ($machine, ELF64, exports: $(echo "$exports" | tr '\n' ' ')GLIBC_${glibc:-none} GLIBCXX_${glibcxx:-none})"
