# Runs mentalfoundry/Pokket's bus-level test suites (MIT, copied unmodified) against this core.
#   make -C tests/pokket check PSEMU_TEST_BIOS=/path/to/kernel.bin
CXX ?= g++
CC ?= gcc
CORE := ../..
CORE_SRCS := $(wildcard $(CORE)/*.cc)
CORE_OBJS := $(patsubst $(CORE)/%.cc,build/core_%.o,$(CORE_SRCS))
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -I$(CORE)
CFLAGS ?= -O2
POKKET_CFLAGS := $(CFLAGS) -std=c11 -Ioverride -I. -DPSEMU_TESTDATA_DIR='"testdata"'

all: build/bu_test

build/core_%.o: $(CORE)/%.cc $(wildcard $(CORE)/*.h $(CORE)/cpu/*.inl $(CORE)/cpu/*/*.inl)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/psemu_shim.o: psemu_shim.cc harness.h psemu/psemu.h $(CORE)/pocketstation.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. -c $< -o $@

build/bu_driver.o: bu_test.c
build/%.o: %.c mock_ps1.h psemu/psemu.h override/assert.h harness.h runner.h
	@mkdir -p build
	$(CC) $(POKKET_CFLAGS) -c $< -o $@

build/bu_test: build/bu_driver.o build/mock_ps1.o build/runner.o build/psemu_shim.o $(CORE_OBJS)
	$(CXX) -o $@ $^

check: build/bu_test
	./build/bu_test

clean:
	rm -rf build

.PHONY: all check clean
