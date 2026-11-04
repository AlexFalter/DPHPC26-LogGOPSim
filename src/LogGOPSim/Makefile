# Compatibility entry point; CMake owns compiler settings and generated sources.
CMAKE ?= cmake
BUILD_DIR ?= ../../build

.PHONY: all configure LogGOPSim txt2bin clean

all: configure
	$(CMAKE) --build "$(BUILD_DIR)" --target LogGOPSim txt2bin

configure:
	@test -f "$(BUILD_DIR)/CMakeCache.txt" || $(CMAKE) -S .. -B "$(BUILD_DIR)"

LogGOPSim txt2bin: configure
	$(CMAKE) --build "$(BUILD_DIR)" --target $@

clean:
	@if test -f "$(BUILD_DIR)/CMakeCache.txt"; then $(CMAKE) --build "$(BUILD_DIR)" --target clean; fi
