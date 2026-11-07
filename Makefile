SHELL := /bin/sh
.DEFAULT_GOAL := all

MPICC ?= mpicc
MPIEXEC ?= mpirun
MPIEXEC_FLAGS ?=
MODE ?= release

ifeq ($(MODE),release)
MODE_CFLAGS := -O2 -g
else ifeq ($(MODE),debug)
MODE_CFLAGS := -O0 -g3
else
$(error MODE must be release or debug)
endif

BUILD_DIR := build/$(MODE)
SMOKE := $(BUILD_DIR)/mpi_smoke
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion

.PHONY: all debug smoke doctor check-compiler check-runner clean help

all: $(SMOKE)

debug:
	$(MAKE) MODE=debug all

$(BUILD_DIR):
	mkdir -p $@

$(SMOKE): src/mpi_smoke.c Makefile | $(BUILD_DIR) check-compiler
	$(MPICC) $(CPPFLAGS) -std=c11 $(WARNINGS) $(MODE_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

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

clean:
	$(RM) -r build

help:
	@printf '%s\n' \
	  'make                         Build the MPI environment check (release).' \
	  'make debug                   Build it with debug symbols and no optimization.' \
	  'make doctor                  Check Open MPI compiler and launcher.' \
	  'make smoke                   Run the environment check with 8 processes.' \
	  'make smoke MODE=debug        Run the debug build.' \
	  'make smoke MPIEXEC_FLAGS=--oversubscribe   Allow 8 ranks on a small local machine.' \
	  'make clean                   Remove local build output.'
