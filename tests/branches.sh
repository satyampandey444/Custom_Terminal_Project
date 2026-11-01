# Verifies numeric tests and nested conditionals.
VALUE=5
if [ "$VALUE" -gt 10 ]; then
    echo SHOULD_NOT_RUN
elif [ "$VALUE" -eq 5 ]; then
    echo MATCH_ELIF
else
    echo SHOULD_NOT_RUN_EITHER
fi
if [ -d "$1" ]; then
    if [ -f "$1/check.sh" ]; then
        echo NESTED_MATCH
    else
        echo NESTED_NO_MATCH
    fi
fi
