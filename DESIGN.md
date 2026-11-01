# Design Notes

## Modular structure

Terminator is organized into C translation units. `main.c` owns the entry point and main X11 event loop. `terminator_internal.h` defines cross-module types and function interfaces. `state.c` defines shared program state exactly once, with declarations in the header.

## GUI and tabs

The GUI module draws the window, prompt, output lines, and tab bar, and manages tab state transitions. X11 calls remain on the main application thread. Tab state stores an explicit working directory so changing directory in one tab does not mutate process-global cwd for other tabs.

## History and synchronization

The history module stores and retrieves commands, loads/saves the history file, implements Ctrl+R search, and supports the process-mode shared history. The current application uses a pthread mutex for in-process shared-history access and a process-shared semaphore for the shared-memory history structure.

## Parser and executor

The parser tokenizes shell input, distinguishes operators from quoted text, handles supported shell expansions and continuation, and builds pipeline command structures. The executor launches external programs and connects pipelines/redirections with `fork`, `execvp`, `pipe`, `open`, and `dup2`.

## Job control

The job module tracks jobs, refreshes process state, routes foreground/background transitions, and provides `jobs`, `fg`, and `bg` support. Process Mode has child-side job management paths in the process module. Signal and process-group behavior should be regression-tested in the target Ubuntu/X11 environment.

## Threads and process mode

The worker module implements per-tab worker threads, command queues, and thread-safe output buffering. Process Mode uses one shell process per tab and framed messages over pipes to communicate with the GUI. Shared history resides in shared memory guarded by a process-shared semaphore.

## multiWatch

The multiWatch module starts commands in separate child processes, reads their output, formats it with the required separators and timestamps, and terminates active processes on Ctrl+C.

## Script interpreter

The script module implements the assignment-defined subset: variable assignments and expansions, positional arguments, `$?`, `if`/`elif`/`else`/`fi`, selected numeric/string/file tests, `exit`, and structural validation before script execution. It is not a full Bash implementation; loops and functions are outside the required subset.

## Design limitations

This document describes the current implementation. Always validate both launch modes with `TESTING.md` on the target Linux/X11 setup.
