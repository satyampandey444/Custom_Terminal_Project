#!/bin/bash
# Verifies variables, positional arguments, and conditional branches.
NAME=World
echo "Hello $NAME"
echo "argument=$1 count=$#"
if [ -f "$1" ]; then
    echo FILE_EXISTS
elif [ -d "$1" ]; then
    echo IS_DIRECTORY
else
    echo MISSING
fi
echo DONE
