# Contributing to guardian

Thank you for your interest in contributing.

## Ground rules

- **Zero warnings.** The project compiles with `-Wall -Wextra -Wpedantic -Werror`.
  Every warning is a build failure. Fix the warning; don't suppress it.
- **Zero dependencies.** Guardian has no runtime dependencies beyond the OS and
  `-lpthread`. New features must be implemented without adding libraries.
- **Both platforms.** Any change that touches process management, IPC, networking,
  or file I/O must be implemented in both `platform_linux.c` and
  `platform_windows.c`. The rest of the codebase must remain `#ifdef`-free.
- **Tests for logic.** New config keys, state machine transitions, or backoff
  calculations should have corresponding tests in `tests/`.

## Development setup

```bash
# Linux / WSL
sudo apt install gcc make
make
make test

# Windows (MinGW required — https://winlibs.com)
make
make test
```

## Code style

- C11 (`-std=c11`)
- 4-space indentation, no tabs
- `snake_case` for all identifiers
- Every function has a brief comment explaining *what* it does and *why*
- `static` for all file-private functions and variables
- No magic numbers — use named constants or `#define`

## Making a change

1. Fork the repository and create a branch from `main`
2. Make your change
3. Run `make && make test` on both Linux and Windows (or WSL for Linux)
4. Open a pull request — the PR template will guide you through the checklist

## Reporting bugs

Use the [bug report template](.github/ISSUE_TEMPLATE/bug_report.md). Include
the output of `guardian version` and the relevant section of your config file.
