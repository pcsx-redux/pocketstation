# Misaligned-load CPU tests (no kernel dump needed).
#   make -C tests/cpu check
CXX ?= g++
CORE := ../..
CORE_SRCS := $(wildcard $(CORE)/*.cc)
CORE_OBJS := $(patsubst $(CORE)/%.cc,build/core_%.o,$(CORE_SRCS))
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -I$(CORE)

all: build/misaligned_loads

build/core_%.o: $(CORE)/%.cc $(wildcard $(CORE)/*.h $(CORE)/cpu/*.inl $(CORE)/cpu/*/*.inl)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/misaligned_loads.o: misaligned_loads.cc $(wildcard $(CORE)/*.h $(CORE)/cpu/*.inl $(CORE)/cpu/*/*.inl)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/misaligned_loads: build/misaligned_loads.o $(CORE_OBJS)
	$(CXX) -o $@ $^

check: build/misaligned_loads
	./build/misaligned_loads

clean:
	rm -rf build

.PHONY: all check clean

build/hw_edge.o: hw_edge.cc $(wildcard $(CORE)/*.h $(CORE)/cpu/*.inl $(CORE)/cpu/*/*.inl)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/hw_edge: build/hw_edge.o $(CORE_OBJS)
	$(CXX) -o $@ $^

check-hw-edge: build/hw_edge
	./build/hw_edge

.PHONY: check-hw-edge
