CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
CORE = $(wildcard ../core/*.cpp)
HEADERS = $(wildcard ../core/*.h) $(wildcard *.h)
BUILD = ../../../.sim/bin

all: $(BUILD)/orion-sim $(BUILD)/bench-looks

$(BUILD)/orion-sim: sim.cpp $(CORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ sim.cpp $(CORE)

$(BUILD)/bench-looks: bench_looks.cpp $(CORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ bench_looks.cpp $(CORE)

.PHONY: all
