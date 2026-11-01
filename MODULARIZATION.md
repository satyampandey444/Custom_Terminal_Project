# Modularization Guide

This version reorganizes the existing working source into separately compiled C modules. The implementation is intended to preserve behavior; the primary change is file organization and shared declarations.

## Files

- `src/main.c` — program entry point, startup and X11 event loop.
- `src/terminator_internal.h` — shared types, constants, state declarations and cross-module function prototypes.
- `src/state.c` — one definition of global/shared state.
- `src/gui.c` — X11 drawing, per-tab screen/input state and tab lifecycle.
- `src/history.c` — persistent history, shared history and Ctrl+R search.
- `src/parser.c` — tokenization, text expansion and multiline continuation helpers.
- `src/executor.c` — pipeline and command dispatch.
- `src/jobs.c` — foreground/background job status and control.
- `src/completion.c` — filename completion.
- `src/hotkeys.c` — configuration parsing and tab hotkeys.
- `src/multiwatch.c` — multiWatch execution, refresh and output formatting.
- `src/process_mode.c` — Process Mode IPC and child-process command loop.
- `src/workers.c` — per-tab worker threads and command/output queues.
- `src/scripts.c` — supported shell-script interpreter subset.
- `src/shell_core.c` — related command-execution and supporting helpers.

## Build

The project uses the same strict compiler settings as the working single-file version:

```bash
make clean
make
```

The Makefile compiles all `.c` translation units and links X11 and pthreads. Shared state has a single definition in `state.c`; other modules access it through `extern` declarations in `terminator_internal.h`.

## Run

```bash
./terminator
./terminator useProcess=True
```

## Important

This is a structural refactor, not a claim that every feature from Bash is implemented. The script interpreter implements the project-defined subset. Run `TESTING.md` in the Ubuntu VM after installing the refactored files, especially to verify concurrency, job control and Process Mode behavior.
