# =============================================================================
# guardian — Makefile
#
# Usage:
#   make              Build the guardian binary (default target)
#   make test         Build and run the test suite
#   make clean        Remove all build artifacts
#   make install      Copy binary to /usr/local/bin (Linux/macOS)
#
# Platform detection:
#   Linux  -> uses src/platform/platform_linux.c,  links -lpthread
#   macOS  -> uses src/platform/platform_macos.c,  links -lpthread
#   Windows (MSVC) -> uses src/platform/platform_windows.c, links ws2_32.lib
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
    TEST_TARGET  = guardian_test.exe
else
    UNAME := $(shell uname -s)
    ifeq ($(UNAME),Linux)
        PLATFORM_SRC = src/platform/platform_linux.c
        LDFLAGS     += -lpthread
        TARGET       = guardian
        TEST_TARGET  = guardian_test
    endif
    ifeq ($(UNAME),Darwin)
        PLATFORM_SRC = src/platform/platform_macos.c
        LDFLAGS     += -lpthread
        TARGET       = guardian
        TEST_TARGET  = guardian_test
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
# Test source files
#
# Tests are compiled into a SEPARATE binary (guardian_test), not into guardian.
# This binary runs all unit tests and exits with 0 on success, non-zero on failure.
# We include the source files under test (config.c, logger.c) so the test
# binary has access to the functions it's testing.
# -----------------------------------------------------------------------------

TEST_SRCS = tests/test_config.c  \
            src/config.c         \
            src/logger.c         \
            $(PLATFORM_SRC)

# Phase 2+ will add:
#   tests/test_service.c  \
#   src/service.c         \

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
#   $^ = all prerequisites (SRCS expanded)
#
# Depends on version-header so the version string is always current.
$(TARGET): version-header $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "  Build successful: $(TARGET)"
	@echo "  Run: ./$(TARGET) version"
	@echo ""

# How to build and run tests:
#   We compile all TEST_SRCS into guardian_test, then run it immediately.
#   If any test fails, the binary exits non-zero, and 'make test' fails too.
#
#   Note: we deliberately exclude src/main.c from tests — main() would
#   conflict with the test runner's own main(). Each test file has its own
#   main() (or a shared one in test_framework.h).
test: $(TEST_SRCS)
	$(CC) $(CFLAGS) $(TEST_SRCS) -o $(TEST_TARGET) $(LDFLAGS)
	@echo ""
	./$(TEST_TARGET)
	@echo ""

# Remove all build artifacts.
# -f tells rm not to error if the files don't exist.
clean:
	rm -f guardian guardian.exe guardian_test guardian_test.exe
	rm -f *.o *.obj *.pdb
	@echo "  Cleaned."

# Copy the binary to /usr/local/bin so you can run 'guardian' from anywhere.
# Requires root/sudo on most systems: 'sudo make install'
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/guardian
	@echo "  Installed guardian to /usr/local/bin/guardian"
