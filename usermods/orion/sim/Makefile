CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
CORE = $(wildcard ../core/*.cpp)
HEADERS = $(wildcard ../core/*.h) $(wildcard *.h)
BUILD = ../../../.sim/bin

all: $(BUILD)/orion-sim $(BUILD)/orion-analyze

$(BUILD)/orion-sim: sim.cpp $(CORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ sim.cpp $(CORE)

$(BUILD)/orion-analyze: analyze.cpp $(CORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ analyze.cpp $(CORE)

.PHONY: all
