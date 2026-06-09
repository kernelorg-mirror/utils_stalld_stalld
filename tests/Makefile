# SPDX-License-Identifier: GPL-2.0-or-later
#
# Makefile for stalld test suite
#
CC	:= gcc
CFLAGS	:= -g -Wall -pthread
LIBS	:= -lpthread

# Test binaries
HELPER_BINS := helpers/starvation_gen
ALL_BINS := $(HELPER_BINS)

.PHONY: all clean test test-functional test-integration

all: $(ALL_BINS)

# Helper binaries
helpers/starvation_gen: helpers/starvation_gen.c
	@mkdir -p helpers
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

# Run all tests
test: all
	@echo "Running test suite..."
	@./run_tests.sh

# Run specific test category
test-functional: $(HELPER_BINS)
	@echo "Running functional tests..."
	@./run_tests.sh --functional-only

test-integration: all
	@echo "Running integration tests..."
	@./run_tests.sh --integration-only

clean:
	@rm -f $(ALL_BINS)
	@# Use sudo for results/ as tests run as root create root-owned log files
	@if [ -d results/ ]; then sudo rm -rf results/; fi
	@rm -f *~ functional/*~ integration/*~ helpers/*~
	@rm -f *.o helpers/*.o
