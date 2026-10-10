CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
CORE = $(wildcard ../core/*.cpp)
HEADERS = $(wildcard ../core/*.h) $(wildcard *.h)
SIMDATA = ../../../.sim
BUILD = $(SIMDATA)/bin

all: $(BUILD)/orion-sim

$(BUILD)/orion-sim: sim.cpp $(CORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ sim.cpp $(CORE)

$(BUILD)/net-test: net_test.cpp $(CORE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ net_test.cpp $(CORE)

# The stock truth file never changes look or palette; this variant does, every phrase.
$(BUILD)/edm128.cues.scores: truth_scores.py $(SIMDATA)/tracks/edm128.json
	python3 truth_scores.py $(SIMDATA)/tracks/edm128.json $@ --looks 4 --palettes 3

net-test: $(BUILD)/net-test $(BUILD)/edm128.cues.scores
	$(BUILD)/net-test --scores $(BUILD)/edm128.cues.scores

.PHONY: all net-test
