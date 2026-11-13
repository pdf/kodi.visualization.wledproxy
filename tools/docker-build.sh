#!/bin/sh
# Builds and tests natively, then cross-builds and packages for aarch64, in
# the Debian trixie container (docker/Dockerfile). Used locally and in CI.
#
#   tools/docker-build.sh            native tests + aarch64 package
#   tools/docker-build.sh test       native tests only
#   tools/docker-build.sh aarch64    aarch64 package only
#
# Results: build-native/ and build-aarch64/ (packages in build-aarch64/).
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
image=${WLEDPROXY_BUILD_IMAGE:-wledproxy-build:trixie}
what=${1:-all}

docker build -q -t "$image" "$here/docker" >/dev/null

run() {
  docker run --rm \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$here:/src" \
    -w /src \
    "$image" sh -euc "$1"
}

if [ "$what" = all ] || [ "$what" = test ]; then
  echo "== native build and tests"
  run 'cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release >/dev/null
       cmake --build build-native -j"$(nproc)"
       cd build-native && ctest --output-on-failure'
fi

if [ "$what" = all ] || [ "$what" = aarch64 ]; then
  echo "== aarch64 build, checks and packages"
  run 'cmake -S . -B build-aarch64 -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake >/dev/null
       cmake --build build-aarch64 -j"$(nproc)"
       cmake --build build-aarch64 --target check_elf
       cmake --build build-aarch64 --target package'
  ls -l "$here"/build-aarch64/visualization.wledproxy-*.tar.gz "$here"/build-aarch64/visualization.wledproxy-*.zip
fi
