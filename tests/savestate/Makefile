# Savestate round trip on OpenPSK (or KERNEL=path/to/kernel.bin).
#   make -C tests/savestate check
CXX ?= g++
CORE := ../..
CORE_SRCS := $(wildcard $(CORE)/*.cc)
CORE_OBJS := $(patsubst $(CORE)/%.cc,build/core_%.o,$(CORE_SRCS))
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -I$(CORE)
KERNEL ?= $(CORE)/openpsk/openpsk.bin

all: build/roundtrip

build/core_%.o: $(CORE)/%.cc $(wildcard $(CORE)/*.h $(CORE)/cpu/*.inl $(CORE)/cpu/*/*.inl)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/roundtrip.o: roundtrip.cc $(wildcard $(CORE)/*.h $(CORE)/cpu/*.inl $(CORE)/cpu/*/*.inl)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/roundtrip: build/roundtrip.o $(CORE_OBJS)
	$(CXX) -o $@ $^

$(CORE)/openpsk/openpsk.bin:
	$(MAKE) -C $(CORE)/openpsk

check: build/roundtrip $(KERNEL)
	./build/roundtrip $(KERNEL)

clean:
	rm -rf build

.PHONY: all check clean
