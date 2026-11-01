Terminator: built-in script interpreter milestone

Build
-----
make clean
make

Run default Thread Mode
-----------------------
./terminator

Run Process Mode
----------------
./terminator useProcess=True

Run these commands INSIDE the Terminator window from the project root:

./tests/check.sh /etc/passwd
bash tests/check.sh /etc/passwd
source tests/check.sh /etc/passwd

./tests/check.sh /tmp
./tests/check.sh /path/that/does/not/exist
./tests/branches.sh /mnt/data/terminator_script_interpreter_bundle/tests
./tests/bad_missing_fi.sh /etc/passwd
./tests/exit_status.sh

Expected checks
---------------
- check.sh should print Hello World, the first argument, and count=1.
- For /etc/passwd, it should print FILE_EXISTS.
- For /tmp, it should print IS_DIRECTORY.
- For an absent path, it should print MISSING.
- branches.sh should print MATCH_ELIF and NESTED_MATCH.
- bad_missing_fi.sh should report a script syntax error and MUST NOT print SHOULD_NOT_RUN.
- exit_status.sh should print BEFORE_EXIT and MUST NOT print AFTER_EXIT_SHOULD_NOT_APPEAR.

Implemented script subset
-------------------------
- Script entry forms: ./script.sh, bash script.sh, source script.sh
- NAME=value assignments scoped to the current script interpreter invocation
- $NAME, ${NAME}, $0, $1..$9, $#, and $?
- if / elif / else / fi, including nesting
- Tests: -eq, -gt, = / ==, !=, -f, -d, plus common numeric comparisons
- exit [n]
- Structural if/then/else/fi validation before any script command executes

Known limitations
-----------------
This is the assignment's required script subset, not a complete Bash implementation.
Loops and functions are not implemented, as the assignment says they are not required.
Advanced Bash expansions, shell-local variable export semantics, compound statements on one line,
and full Bash parsing behavior are not supported.
