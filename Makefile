CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -g -pthread
LDFLAGS = -lX11 -pthread

TARGET = terminator
SRC = src/main.c \
    src/state.c \
    src/completion.c \
    src/executor.c \
    src/gui.c \
    src/history.c \
    src/hotkeys.c \
    src/jobs.c \
    src/multiwatch.c \
    src/parser.c \
    src/process_mode.c \
    src/scripts.c \
    src/shell_core.c \
    src/workers.c

all: $(TARGET)

$(TARGET): $(SRC) src/terminator_internal.h
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
