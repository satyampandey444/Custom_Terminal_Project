#include "terminator_internal.h"

/* Multiwatch module. */

static volatile sig_atomic_t process_multiwatch_interrupted = 0;

int parse_multiwatch_invocation(const char *line,
                                       char commands[MAX_WATCH_COMMANDS][INPUT_CAPACITY],
                                       int *command_count)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    const char *word = "multiWatch";
    size_t word_len = strlen(word);
    if (strncmp(p, word, word_len) != 0 ||
        (p[word_len] != '\0' && p[word_len] != ' ' && p[word_len] != '\t' && p[word_len] != '['))
        return 0;
    p += word_len;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[')
        return -1;
    p++;
    int count = 0;

    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == ']') {
            p++;
            break;
        }
        if (*p != '\'' && *p != '"')
            return -1;
        if (count >= MAX_WATCH_COMMANDS)
            return -1;

        char quote = *p++;
        size_t used = 0;
        int closed = 0;
        while (*p != '\0') {
            if (*p == quote) {
                p++;
                closed = 1;
                break;
            }
            char c = *p++;
            if (c == '\\') {
                if (*p == '\0')
                    return -1;
                c = *p++;
            }
            if (used + 1 >= INPUT_CAPACITY)
                return -1;
            commands[count][used++] = c;
        }
        if (!closed || used == 0)
            return -1;
        commands[count][used] = '\0';
        count++;

        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            p++;
            break;
        }
        return -1;
    }

    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '\0' || count == 0)
        return -1;
    *command_count = count;
    return 1;
}

int execute_pipeline_to_fd(ParsedCommand commands[], int count, int target_fd)
{
    int pipes[MAX_COMMANDS - 1][2];
    pid_t pids[MAX_COMMANDS];
    int launched = 0;

    if (count < 1 || count > MAX_COMMANDS)
        return 2;

    for (int i = 0; i < count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            dprintf(target_fd, "multiWatch: pipe failed: %s\n", strerror(errno));
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return 1;
        }
    }

    for (int i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            dprintf(target_fd, "multiWatch: fork failed: %s\n", strerror(errno));
            break;
        }
        if (pid == 0) {
            if (enter_active_tab_directory() != 0)
                _exit(126);
            (void)signal(SIGINT, SIG_DFL);
            (void)signal(SIGTSTP, SIG_DFL);
            (void)signal(SIGTERM, SIG_DFL);
            (void)signal(SIGPIPE, SIG_DFL);

            if (dup2(target_fd, STDERR_FILENO) == -1)
                _exit(126);

            int in_fd = STDIN_FILENO;
            int out_fd = STDOUT_FILENO;
            if (commands[i].infile != NULL) {
                in_fd = open(commands[i].infile, O_RDONLY);
                if (in_fd == -1) {
                    dprintf(STDERR_FILENO, "terminator: cannot open %s: %s\n",
                            commands[i].infile, strerror(errno));
                    _exit(1);
                }
            } else if (i > 0) {
                in_fd = pipes[i - 1][0];
            }

            if (commands[i].outfile != NULL) {
                out_fd = open(commands[i].outfile,
                              O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (out_fd == -1) {
                    dprintf(STDERR_FILENO, "terminator: cannot open %s: %s\n",
                            commands[i].outfile, strerror(errno));
                    _exit(1);
                }
            } else if (i < count - 1) {
                out_fd = pipes[i][1];
            } else {
                out_fd = target_fd;
            }

            if (dup2(in_fd, STDIN_FILENO) == -1 ||
                dup2(out_fd, STDOUT_FILENO) == -1) {
                dprintf(STDERR_FILENO, "multiWatch: dup2 failed: %s\n", strerror(errno));
                _exit(126);
            }
            if (in_fd != STDIN_FILENO) close(in_fd);
            if (out_fd != STDOUT_FILENO) close(out_fd);
            for (int j = 0; j < count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            if (target_fd != STDOUT_FILENO && target_fd != STDERR_FILENO)
                close(target_fd);

            execvp(commands[i].argv[0], commands[i].argv);
            dprintf(STDERR_FILENO, "terminator: %s: command not found\n",
                    commands[i].argv[0]);
            _exit(127);
        }
        pids[launched++] = pid;
    }

    for (int i = 0; i < count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    int result_status = 0;
    for (int i = 0; i < launched; i++) {
        int status = 0;
        while (waitpid(pids[i], &status, 0) == -1) {
            if (errno == EINTR) continue;
            status = 1 << 8;
            break;
        }
        if (i == launched - 1) {
            if (WIFEXITED(status)) result_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) result_status = 128 + WTERMSIG(status);
            else result_status = 1;
        }
    }
    return result_status;
}

int execute_watch_command(const char *raw_command, int target_fd)
{
    char expanded[MAX_COMMAND_SIZE];
    if (expand_shell_text(raw_command, expanded, sizeof(expanded), 0) != 0) {
        dprintf(target_fd, "multiWatch: command expansion failed or was too large\n");
        return 2;
    }

    Token tokens[MAX_TOKENS];
    int token_count = 0;
    if (tokenize(expanded, tokens, &token_count) != 0 || token_count == 0) {
        dprintf(target_fd, "multiWatch: could not parse command\n");
        return 2;
    }

    ParsedCommand parsed[MAX_COMMANDS];
    int parsed_count = 0;
    if (parse_command(tokens, token_count, parsed, &parsed_count) != 0) {
        dprintf(target_fd, "multiWatch: invalid command syntax\n");
        return 2;
    }

    return execute_pipeline_to_fd(parsed, parsed_count, target_fd);
}

int start_watch_cycle(WatchCommand *watch)
{
    int fds[2];
    if (pipe(fds) == -1) {
        add_text("multiWatch: could not create output pipe\n");
        watch->next_start_at = time(NULL) + WATCH_INTERVAL_SECONDS;
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(fds[0]);
        close(fds[1]);
        add_text("multiWatch: could not start command\n");
        watch->next_start_at = time(NULL) + WATCH_INTERVAL_SECONDS;
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (enter_active_tab_directory() != 0)
            _exit(126);
        (void)setpgid(0, 0);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGTSTP, SIG_DFL);
        (void)signal(SIGTERM, SIG_DFL);
        int status = execute_watch_command(watch->command, fds[1]);
        close(fds[1]);
        _exit(status == 0 ? 0 : 1);
    }

    (void)setpgid(pid, pid);
    close(fds[1]);
    watch->pid = pid;
    watch->output_fd = fds[0];
    watch->cycle_active = 1;
    watch->pipe_eof = 0;
    watch->worker_done = 0;
    watch->output_truncated = 0;
    watch->started_at = time(NULL);
    watch->output_length = 0;
    watch->output[0] = '\0';
    watch->next_start_at = 0;
    return 0;
}

void emit_watch_piece(WatchCommand *watch, const char *data, size_t length)
{
    char header[INPUT_CAPACITY + 96];
    snprintf(header, sizeof(header), "\"%.4095s\", %lld:\n%s",
             watch->command, (long long)watch->started_at, WATCH_SEPARATOR);
    add_text(header);
    for (size_t i = 0; i < length; i++)
        add_char(data[i]);
    if (length == 0 || data[length - 1] != '\n')
        add_char('\n');
    add_text(WATCH_SEPARATOR);
}

void finish_watch_cycle(WatchCommand *watch)
{
    /* A silent command still produces an empty formatted result block. */
    if (watch->output_length == 0) {
        emit_watch_piece(watch, "", 0);
    }

    if (watch->output_fd >= 0) close(watch->output_fd);
    watch->output_fd = -1;
    watch->pid = 0;
    watch->cycle_active = 0;
    watch->pipe_eof = 0;
    watch->worker_done = 0;
    watch->next_start_at = time(NULL) + WATCH_INTERVAL_SECONDS;
    draw_window();
}

void terminate_watch_commands(WatchCommand watches[], int count)
{
    for (int i = 0; i < count; i++) {
        if (watches[i].cycle_active && watches[i].pid > 0)
            (void)kill(-watches[i].pid, SIGTERM);
    }
    struct timespec grace = {0, 150000000L};
    (void)nanosleep(&grace, NULL);
    for (int i = 0; i < count; i++) {
        if (watches[i].cycle_active && watches[i].pid > 0)
            (void)kill(-watches[i].pid, SIGKILL);
    }
    for (int i = 0; i < count; i++) {
        if (watches[i].pid > 0) {
            while (waitpid(watches[i].pid, NULL, 0) == -1 && errno == EINTR) {}
        }
        if (watches[i].output_fd >= 0)
            close(watches[i].output_fd);
        watches[i].output_fd = -1;
        watches[i].pid = 0;
        watches[i].cycle_active = 0;
    }
}

int run_multiwatch(char commands[MAX_WATCH_COMMANDS][INPUT_CAPACITY], int count)
{
    WatchCommand watches[MAX_WATCH_COMMANDS];
    memset(watches, 0, sizeof(watches));
    for (int i = 0; i < count; i++) {
        snprintf(watches[i].command, sizeof(watches[i].command), "%.4095s", commands[i]);
        watches[i].output_fd = -1;
        watches[i].next_start_at = time(NULL);
    }

    add_text("multiWatch running. Press Ctrl+C to stop.\n");
    draw_window();
    int interrupted = 0;

    while (running && !interrupted) {
        time_t now = time(NULL);
        for (int i = 0; i < count; i++) {
            if (!watches[i].cycle_active && now >= watches[i].next_start_at)
                (void)start_watch_cycle(&watches[i]);
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        int xfd = ConnectionNumber(display);
        FD_SET(xfd, &readfds);
        int maxfd = xfd;
        for (int i = 0; i < count; i++) {
            int fd = watches[i].output_fd;
            if (watches[i].cycle_active && fd >= 0 && fd < FD_SETSIZE) {
                FD_SET(fd, &readfds);
                if (fd > maxfd) maxfd = fd;
            }
        }
        struct timeval timeout = {0, 200000};
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0 && errno != EINTR) {
            add_text("multiWatch: select failed\n");
            interrupted = 1;
            break;
        }

        /* Consume all X events; only Ctrl+C has command meaning while watching. */
        if (XPending(display) > 0 || (ready > 0 && FD_ISSET(xfd, &readfds))) {
            while (XPending(display) > 0) {
                XEvent event;
                XNextEvent(display, &event);
                if (event.type == Expose) {
                    draw_window();
                } else if (event.type == KeyPress) {
                    char chars[32];
                    KeySym key;
                    XLookupString(&event.xkey, chars, sizeof(chars), &key, NULL);
                    int control = (event.xkey.state & ControlMask) != 0;
                    if (control && (key == XK_c || key == XK_C)) {
                        add_text("^C\n");
                        interrupted = 1;
                        break;
                    }
                }
            }
        }
        if (interrupted) break;

        if (ready > 0) {
            for (int i = 0; i < count; i++) {
                WatchCommand *watch = &watches[i];
                int fd = watch->output_fd;
                if (!watch->cycle_active || fd < 0 || fd >= FD_SETSIZE || !FD_ISSET(fd, &readfds))
                    continue;
                char buffer[4096];
                ssize_t n;
                do {
                    n = read(fd, buffer, sizeof(buffer));
                } while (n < 0 && errno == EINTR);
                if (n > 0) {
                    emit_watch_piece(watch, buffer, (size_t)n);
                    watch->output_length += (size_t)n;
                    draw_window();
                } else if (n == 0) {
                    close(watch->output_fd);
                    watch->output_fd = -1;
                    watch->pipe_eof = 1;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(watch->output_fd);
                    watch->output_fd = -1;
                    watch->pipe_eof = 1;
                }
            }
        }

        for (int i = 0; i < count; i++) {
            WatchCommand *watch = &watches[i];
            if (!watch->cycle_active) continue;
            if (!watch->worker_done && watch->pid > 0) {
                pid_t result = waitpid(watch->pid, NULL, WNOHANG);
                if (result == watch->pid || (result == -1 && errno == ECHILD))
                    watch->worker_done = 1;
            }
            if (watch->worker_done && watch->pipe_eof)
                finish_watch_cycle(watch);
        }
        draw_window();
    }

    terminate_watch_commands(watches, count);
    if (interrupted) {
        draw_window();
        last_exit_status = 130;
        return 130;
    }
    last_exit_status = 0;
    return 0;
}

void process_multiwatch_sigint_handler(int signo)
{
    (void)signo;
    process_multiwatch_interrupted = 1;
}

int run_multiwatch_process_mode(int slot,
                                      char commands[MAX_WATCH_COMMANDS][INPUT_CAPACITY],
                                      int count)
{
    if (count < 1 || count > MAX_WATCH_COMMANDS) {
        add_text("multiWatch: expected at least one command\n");
        return 2;
    }

    WatchCommand watches[MAX_WATCH_COMMANDS];
    memset(watches, 0, sizeof(watches));
    for (int i = 0; i < count; i++) {
        snprintf(watches[i].command, sizeof(watches[i].command), "%.4095s", commands[i]);
        watches[i].output_fd = -1;
        watches[i].next_start_at = time(NULL);
    }

    struct sigaction old_action;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = process_multiwatch_sigint_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, &old_action) == -1) {
        add_text("multiWatch: could not install SIGINT handler\n");
        return 1;
    }

    process_multiwatch_interrupted = 0;
    worker_set_active_pgid(slot, getpgrp());
    add_text("multiWatch running. Press Ctrl+C to stop.\n");

    while (!process_multiwatch_interrupted) {
        time_t now = time(NULL);
        for (int i = 0; i < count; i++) {
            if (!watches[i].cycle_active && now >= watches[i].next_start_at) {
                if (start_watch_cycle(&watches[i]) == 0) {
                    int flags = fcntl(watches[i].output_fd, F_GETFL, 0);
                    if (flags >= 0)
                        (void)fcntl(watches[i].output_fd, F_SETFL, flags | O_NONBLOCK);
                }
            }
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;
        for (int i = 0; i < count; i++) {
            int fd = watches[i].output_fd;
            if (watches[i].cycle_active && fd >= 0 && fd < FD_SETSIZE) {
                FD_SET(fd, &readfds);
                if (fd > maxfd) maxfd = fd;
            }
        }

        struct timeval timeout = {0, 200000};
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR && process_multiwatch_interrupted)
                break;
            if (errno == EINTR)
                continue;
            add_text("multiWatch: select failed\n");
            break;
        }

        if (ready > 0) {
            for (int i = 0; i < count; i++) {
                WatchCommand *watch = &watches[i];
                int fd = watch->output_fd;
                if (!watch->cycle_active || fd < 0 || fd >= FD_SETSIZE ||
                    !FD_ISSET(fd, &readfds))
                    continue;

                for (;;) {
                    char buffer[4096];
                    ssize_t n = read(fd, buffer, sizeof(buffer));
                    if (n > 0) {
                        emit_watch_piece(watch, buffer, (size_t)n);
                        watch->output_length += (size_t)n;
                        continue;
                    }
                    if (n == 0) {
                        close(watch->output_fd);
                        watch->output_fd = -1;
                        watch->pipe_eof = 1;
                        break;
                    }
                    if (errno == EINTR) {
                        if (process_multiwatch_interrupted) break;
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    close(watch->output_fd);
                    watch->output_fd = -1;
                    watch->pipe_eof = 1;
                    break;
                }
            }
        }

        now = time(NULL);
        for (int i = 0; i < count; i++) {
            WatchCommand *watch = &watches[i];
            if (!watch->cycle_active)
                continue;
            if (!watch->worker_done && watch->pid > 0) {
                pid_t result = waitpid(watch->pid, NULL, WNOHANG);
                if (result == watch->pid || (result == -1 && errno == ECHILD))
                    watch->worker_done = 1;
            }
            if (watch->worker_done && watch->pipe_eof) {
                if (watch->output_length == 0)
                    emit_watch_piece(watch, "", 0);
                watch->pid = 0;
                watch->cycle_active = 0;
                watch->pipe_eof = 0;
                watch->worker_done = 0;
                watch->output_truncated = 0;
                watch->output_length = 0;
                watch->output[0] = '\0';
                watch->next_start_at = now + WATCH_INTERVAL_SECONDS;
            }
        }
    }

    terminate_watch_commands(watches, count);
    worker_set_active_pgid(slot, 0);
    (void)sigaction(SIGINT, &old_action, NULL);

    if (process_multiwatch_interrupted) {
        add_text("multiWatch stopped\n");
        last_exit_status = 130;
        return 130;
    }
    last_exit_status = 0;
    return 0;
}
