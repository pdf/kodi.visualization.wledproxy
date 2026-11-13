# Convenience targets. The real build is CMake; see README.md.
#
#   make test      native build and tests on this machine
#   make docker    native tests and aarch64 package in the build container
#   make aarch64   aarch64 package in the build container
#   make clean

BUILD ?= build

.PHONY: all test docker aarch64 clean

all: test

test:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j
	cd $(BUILD) && ctest --output-on-failure

docker:
	tools/docker-build.sh all

aarch64:
	tools/docker-build.sh aarch64

clean:
	rm -rf $(BUILD) build-native build-aarch64 build-dbg
