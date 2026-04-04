# =============================================================================
# guardian — Makefile
#
# Usage:
#   make              Build the guardian binary (default target)
#   make test         Build and run all test suites
#   make clean        Remove all build artifacts
#   make install      Copy binary to /usr/local/bin (Linux/macOS)
#
# Platform detection:
#   Linux  -> uses src/platform/platform_linux.c,  links -lpthread
#   macOS  -> uses src/platform/platform_macos.c,  links -lpthread
#   Windows (MinGW) -> uses src/platform/platform_windows.c, links ws2_32
# =============================================================================

# -----------------------------------------------------------------------------
# Compiler and base flags
# -----------------------------------------------------------------------------

CC     = gcc

# Compiler flags explained:
#   -std=c11        Compile as C11 (enables _Atomic, _Static_assert, etc.)
#   -Wall           Enable all commonly useful warnings
#   -Wextra         Enable extra warnings beyond -Wall
#   -Wpedantic      Enforce strict ISO C compliance (catches non-portable code)
#   -Werror         Treat ALL warnings as errors — you cannot build with warnings
#   -Isrc           Add src/ to the include search path so we can write
#                   #include "logger.h" instead of #include "../src/logger.h"
#   -g              Include debug symbols (for gdb/lldb debugging)
#
# Why -Werror? Because warnings are bugs waiting to happen. At Google and
# Microsoft, -Werror (or equivalent) is mandatory. It forces you to address
# every warning immediately rather than accumulating technical debt.
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc -g

# Linker flags — additional libraries to link against.
# Set per-platform below.
LDFLAGS =

# -----------------------------------------------------------------------------
# Platform detection
#
# We use two signals:
#   $(OS)   — set to "Windows_NT" by Windows itself for ALL shells (cmd,
#             PowerShell, Git Bash, Cygwin). The most reliable Windows check.
#   uname -s — returns "Linux" or "Darwin" on those platforms.
#             On Git Bash it returns "MINGW64_NT-..." which is why we don't
#             rely on uname for Windows detection.
# -----------------------------------------------------------------------------

ifeq ($(OS),Windows_NT)
    # Windows — using GCC via MinGW with the Win32 API implementation.
    # platform_windows.c uses CreateProcess, Job Objects, and WaitForSingleObject.
    # Links against: -lpthread (for future threading), ws2_32 (Phase 5 networking)
    PLATFORM_SRC = src/platform/platform_windows.c
    LDFLAGS     += -lpthread -lws2_32
    TARGET       = guardian.exe
    TEST_EXT     = .exe
else
    UNAME := $(shell uname -s)
    ifeq ($(UNAME),Linux)
        PLATFORM_SRC = src/platform/platform_linux.c
        LDFLAGS     += -lpthread
        TARGET       = guardian
        TEST_EXT     =
    endif
    ifeq ($(UNAME),Darwin)
        PLATFORM_SRC = src/platform/platform_macos.c
        LDFLAGS     += -lpthread
        TARGET       = guardian
        TEST_EXT     =
    endif
endif

# -----------------------------------------------------------------------------
# Source files
#
# These are the .c files that make up the main guardian binary.
# Each file is compiled separately, then linked together.
# The platform-specific file (PLATFORM_SRC) is added based on the OS.
# -----------------------------------------------------------------------------

SRCS = src/main.c        \
       src/cli.c         \
       src/config.c      \
       src/logger.c      \
       src/service.c     \
       src/supervisor.c  \
       src/ipc.c         \
       src/health.c      \
       $(PLATFORM_SRC)

# -----------------------------------------------------------------------------
# Test binaries
#
# Each test suite is a SEPARATE binary so that:
#   1. Each has its own main() — no conflicts.
#   2. Each binary only links what it needs (e.g. test_logger doesn't
#      need the platform file at all — logger.c uses only standard C).
#   3. A compilation error in one suite doesn't block the others.
#
# Note: we deliberately exclude src/main.c from all test binaries —
# main() would conflict with the test file's own main().
# -----------------------------------------------------------------------------

# Config parser tests: needs config.c, logger.c, and the platform file
# (config.c → service.h → platform.h, so the platform obj must link).
TEST_CONFIG  = guardian_test_config$(TEST_EXT)

# Service logic tests: service_state_name() + service_compute_backoff_ms()
# Needs service.c (the code under test), logger.c (used by service.c), platform.
TEST_SERVICE = guardian_test_service$(TEST_EXT)

# Logger tests: only logger.c — no platform file needed.
# logger.c uses only standard C (stdio, time, stdarg). This makes it the
# purest unit test in the suite: zero OS dependencies.
TEST_LOGGER  = guardian_test_logger$(TEST_EXT)

# Health queue tests: circular buffer push/pop/init from health.c.
# Needs health.c (the code under test), logger.c, platform.
TEST_HEALTH  = guardian_test_health$(TEST_EXT)

# All test binaries in one variable for the 'test' target dependency.
TEST_BINARIES = $(TEST_CONFIG) $(TEST_SERVICE) $(TEST_LOGGER) $(TEST_HEALTH)

# -----------------------------------------------------------------------------
# Build rules
# -----------------------------------------------------------------------------

# .PHONY declares targets that are NOT files on disk.
# Without this, if you ever create a file named "clean" or "test",
# make would think the target is up-to-date and refuse to run it.
.PHONY: all clean test install version-header

# 'all' is the default target (first non-special target in the file).
# Running just 'make' is equivalent to 'make all'.
all: $(TARGET)

# Generate src/version.h from git describe before each build.
# We try python3 first, then python (Windows often ships only 'python').
# If neither is available, we keep any existing version.h unchanged.
# The '-' prefix suppresses make errors if the command fails.
version-header:
	-python3 tools/gen_version.py 2>/dev/null || python tools/gen_version.py 2>/dev/null

# How to build the guardian binary:
#   $@ = the target name (guardian or guardian.exe)
#
# Depends on version-header so the version string is always current.
# We use $(SRCS) explicitly instead of $^ to exclude the phony
# version-header prerequisite from the compiler's input list.
$(TARGET): version-header $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "  Build successful: $(TARGET)"
	@echo "  Run: ./$(TARGET) version"
	@echo ""

# -----------------------------------------------------------------------------
# Test binary build rules
# -----------------------------------------------------------------------------

$(TEST_CONFIG): tests/test_config.c src/config.c src/logger.c $(PLATFORM_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_SERVICE): tests/test_service.c src/service.c src/logger.c $(PLATFORM_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_LOGGER): tests/test_logger.c src/logger.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_HEALTH): tests/test_health_queue.c src/health.c src/logger.c $(PLATFORM_SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# -----------------------------------------------------------------------------
# Test runner
#
# Builds all test binaries then runs them in sequence.
# If any binary exits non-zero (a test failed), make stops immediately
# and reports failure — standard shell semantics with no '||true' hacks.
# -----------------------------------------------------------------------------

test: $(TEST_BINARIES)
	@echo ""
	@echo "  ── Config parser tests ────────────────────────────────────────"
	./$(TEST_CONFIG)
	@echo "  ── Service logic tests ────────────────────────────────────────"
	./$(TEST_SERVICE)
	@echo "  ── Logger tests ───────────────────────────────────────────────"
	./$(TEST_LOGGER)
	@echo "  ── Health queue tests ─────────────────────────────────────────"
	./$(TEST_HEALTH)
	@echo ""
	@echo "  All test suites passed."
	@echo ""

# Remove all build artifacts.
# -f tells rm not to error if the files don't exist.
clean:
	rm -f guardian guardian.exe
	rm -f guardian_test_config guardian_test_config.exe
	rm -f guardian_test_service guardian_test_service.exe
	rm -f guardian_test_logger  guardian_test_logger.exe
	rm -f guardian_test_health  guardian_test_health.exe
	rm -f *.o *.obj *.pdb
	@echo "  Cleaned."

# Copy the binary to /usr/local/bin so you can run 'guardian' from anywhere.
# Requires root/sudo on most systems: 'sudo make install'
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/guardian
	@echo "  Installed guardian to /usr/local/bin/guardian"
