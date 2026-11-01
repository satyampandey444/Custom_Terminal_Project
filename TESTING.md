# Terminator Regression Tests

Run from the project root in both modes (`./terminator` and `./terminator useProcess=True`).

## Basic execution

```text
pwd
whoami
echo Hello World
ls
```

## Quotes and expansion

```text
echo "$HOME"
echo '$HOME'
echo $(basename $(pwd))
echo Hello | wc -c
```

## Redirection and pipelines

```text
echo test > /tmp/terminator-test.txt
cat < /tmp/terminator-test.txt
cat /etc/passwd | sort | head -5
```

## Job control

```text
sleep 30 &
jobs
sleep 100
```

Press Ctrl+Z, check `jobs`, run `bg %N`, then `fg %N` and press Ctrl+C. Substitute the actual job ID.

## multiWatch

```text
multiWatch ["date +%T", "ls | wc -l"]
```

Press Ctrl+C and verify the prompt returns.

## Scripts

```text
./tests/check.sh /etc/passwd
./tests/branches.sh tests
./tests/exit_status.sh
./tests/bad_missing_fi.sh /etc/passwd
```

The malformed script must be rejected before its body executes.
