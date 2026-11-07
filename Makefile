SHELL := /bin/sh
.DEFAULT_GOAL := all

MPICC ?= mpicc
MPIEXEC ?= mpirun
MPIEXEC_FLAGS ?=
ARGS ?=
MODE ?= release

ifeq ($(MODE),release)
MODE_CFLAGS := -O2 -g
else ifeq ($(MODE),debug)
MODE_CFLAGS := -O0 -g3
else ifeq ($(MODE),sanitize)
MODE_CFLAGS := -O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
else
$(error MODE must be release, debug or sanitize)
endif

BUILD_DIR := build/$(MODE)
SMOKE := $(BUILD_DIR)/mpi_smoke
MERGE := $(BUILD_DIR)/merge
MERGE_TEST := $(BUILD_DIR)/merge_test
MERGE_SOURCES := src/main.c src/merge.c src/datasets.c
MERGE_HEADERS := src/merge.h src/datasets.h src/mpi_helpers.h
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion

.PHONY: all debug smoke run test doctor check-compiler check-runner clean help

all: $(SMOKE) $(MERGE)

debug:
	$(MAKE) MODE=debug all

$(BUILD_DIR):
	mkdir -p $@

$(SMOKE): src/mpi_smoke.c Makefile | $(BUILD_DIR) check-compiler
	$(MPICC) $(CPPFLAGS) -std=c11 $(WARNINGS) $(MODE_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

$(MERGE): $(MERGE_SOURCES) $(MERGE_HEADERS) Makefile | $(BUILD_DIR) check-compiler
	$(MPICC) $(CPPFLAGS) -std=c11 $(WARNINGS) $(MODE_CFLAGS) $(CFLAGS) $(MERGE_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(MERGE_TEST): $(MERGE_SOURCES) $(MERGE_HEADERS) Makefile | $(BUILD_DIR) check-compiler
	$(MPICC) $(CPPFLAGS) -DMERGE_SELF_TEST -std=c11 $(WARNINGS) $(MODE_CFLAGS) $(CFLAGS) $(MERGE_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

check-compiler:
	@command -v $(MPICC) >/dev/null 2>&1 || { \
		printf '%s\n' 'Open MPI compiler not found: $(MPICC). Check your PATH or set MPICC.' >&2; exit 1; }
	@version="$$($(MPICC) --showme:version 2>&1)"; \
	case "$$version" in *"Open MPI"*) ;; \
	*) printf '%s\n' 'Expected an Open MPI compiler wrapper:' "$$version" >&2; exit 1 ;; esac

check-runner:
	@command -v $(MPIEXEC) >/dev/null 2>&1 || { \
		printf '%s\n' 'Open MPI launcher not found: $(MPIEXEC). Check your PATH or set MPIEXEC.' >&2; exit 1; }
	@version="$$($(MPIEXEC) --version 2>&1)"; \
	case "$$version" in *"Open MPI"*|*"OpenRTE"*) ;; \
	*) printf '%s\n' 'Expected an Open MPI launcher:' "$$version" >&2; exit 1 ;; esac

doctor: check-compiler check-runner
	@command -v $(MPICC)
	@$(MPICC) --showme:version
	@$(MPICC) --showme:command
	@command -v $(MPIEXEC)
	@$(MPIEXEC) --version

smoke: $(SMOKE) check-runner
	$(MPIEXEC) $(MPIEXEC_FLAGS) -np 8 ./$(SMOKE)

run: $(MERGE) check-runner
	$(MPIEXEC) $(MPIEXEC_FLAGS) -np 8 ./$(MERGE) $(ARGS)

test: $(MERGE_TEST) check-runner
	$(MPIEXEC) $(MPIEXEC_FLAGS) -np 8 ./$(MERGE_TEST)

clean:
	$(RM) -r build

help:
	@printf '%s\n' \
	  'make                         Build the merge and environment check (release).' \
	  'make debug                   Build both programs without optimization.' \
	  'make doctor                  Check Open MPI compiler and launcher.' \
	  'make run ARGS="4 alternating --trace"   Run the merge.' \
	  'make test                    Run 72 distributed correctness cases.' \
	  'make test MODE=debug         Test the debug build.' \
	  'make test MODE=sanitize      Test with AddressSanitizer and UBSan.' \
	  'make smoke                   Run the environment check with 8 processes.' \
	  'make smoke MODE=debug        Run the debug build.' \
	  'make smoke MPIEXEC_FLAGS=--oversubscribe   Allow 8 ranks on a small local machine.' \
	  'make clean                   Remove local build output.'
