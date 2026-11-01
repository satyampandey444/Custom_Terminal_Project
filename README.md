# Terminator — Custom X11 Terminal

A C terminal emulator and shell prototype for Linux, built with Xlib and POSIX APIs.

## Build

Dependencies: GCC, GNU Make, X11 development libraries, and pthread support. On Ubuntu:

```bash
sudo apt update
sudo apt install build-essential libx11-dev x11-apps
make clean && make
```

## Run

```bash
./terminator
./terminator useProcess=True
```

## Source layout

- `main.c`: application entry point and X11 event loop.
- `terminator_internal.h`: shared types, constants, state declarations, and interfaces.
- `state.c`: one definition for shared application state.
- `gui.c`: drawing, per-tab UI state, and tab lifecycle.
- `history.c`: persistent/shared history and history search.
- `parser.c`: tokenization, expansions, and continuation handling.
- `executor.c`: command parsing/execution dispatch.
- `jobs.c`: job state and foreground/background control helpers.
- `process_mode.c`: process-mode IPC and child-side dispatch.
- `workers.c`: per-tab thread workers and command submission.
- `multiwatch.c`: multiWatch execution and output formatting.
- `completion.c`: filename completion.
- `hotkeys.c`: configurable tab shortcuts.
- `scripts.c`: supported subset of the shell-script interpreter.
- `shell_core.c`: supporting shell helpers not assigned elsewhere.

## Notes

This refactor preserves the current implementation while moving function definitions into focused translation units. The script engine implements the assignment's required subset, not all Bash syntax. Review `DESIGN.md` and `TESTING.md` before submission.
