#include "terminator_internal.h"

/* Executor module. */

int execute_pipeline_internal(ParsedCommand commands[], int count,
                                     char *capture, size_t capture_capacity)
{
    size_t captured_len = 0;
    if (capture != NULL && capture_capacity > 0)
        capture[0] = '\0';
    int pipes[MAX_COMMANDS - 1][2];
    int output_pipe[2];
    pid_t pids[MAX_COMMANDS];
    int launched = 0;
    pid_t worker_pgid = 0;
    int worker_managed = (worker_context_slot >= 0 && capture == NULL);

    for (int i = 0; i < count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            if (capture == NULL)
                add_text("terminator: pipe failed\n");

            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return 1;
        }
    }

    if (pipe(output_pipe) == -1) {
        if (capture == NULL)
            add_text("terminator: output pipe failed\n");

        for (int i = 0; i < count - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        return 1;
    }

    for (int i = 0; i < count; i++) {
        pid_t pid = fork();

        if (pid == -1) {
            if (capture == NULL)
                add_text("terminator: fork failed\n");
            break;
        }

        if (pid == 0) {
            if (worker_managed) {
                if (worker_pgid == 0)
                    (void)setpgid(0, 0);
                else
                    (void)setpgid(0, worker_pgid);
                (void)signal(SIGINT, SIG_DFL);
                (void)signal(SIGTSTP, SIG_DFL);
                (void)signal(SIGQUIT, SIG_DFL);
                (void)signal(SIGPIPE, SIG_DFL);
            }
            if (enter_active_tab_directory() != 0)
                _exit(126);
            /* Send error messages to the GUI output pipe. */
            if (dup2(output_pipe[1], STDERR_FILENO) == -1)
                _exit(126);

            int in_fd = STDIN_FILENO;
            int out_fd = STDOUT_FILENO;

            if (commands[i].infile != NULL) {
                in_fd = open(commands[i].infile, O_RDONLY);

                if (in_fd == -1) {
                    dprintf(STDERR_FILENO,
                            "terminator: cannot open %s: %s\n",
                            commands[i].infile, strerror(errno));
                    _exit(1);
                }
            } else if (i > 0) {
                in_fd = pipes[i - 1][0];
            }

            if (commands[i].outfile != NULL) {
                out_fd = open(commands[i].outfile,
                              O_WRONLY | O_CREAT | O_TRUNC,
                              0666);

                if (out_fd == -1) {
                    dprintf(STDERR_FILENO,
                            "terminator: cannot open %s: %s\n",
                            commands[i].outfile, strerror(errno));
                    _exit(1);
                }
            } else if (i < count - 1) {
                out_fd = pipes[i][1];
            } else {
                out_fd = output_pipe[1];
            }

            if (dup2(in_fd, STDIN_FILENO) == -1 ||
                dup2(out_fd, STDOUT_FILENO) == -1) {
                dprintf(STDERR_FILENO,
                        "terminator: dup2 failed: %s\n",
                        strerror(errno));
                _exit(126);
            }

            if (in_fd != STDIN_FILENO)
                close(in_fd);

            if (out_fd != STDOUT_FILENO)
                close(out_fd);

            for (int j = 0; j < count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            close(output_pipe[0]);
            close(output_pipe[1]);

            execvp(commands[i].argv[0], commands[i].argv);

            dprintf(STDERR_FILENO,
                    "terminator: %s: command not found\n",
                    commands[i].argv[0]);

            _exit(127);
        }

        if (worker_managed) {
            if (worker_pgid == 0)
                worker_pgid = pid;
            (void)setpgid(pid, worker_pgid);
            worker_set_active_pgid(worker_context_slot, worker_pgid);
        }
        pids[launched++] = pid;
    }

    /* Parent must close its copies so children can receive EOF. */
    for (int i = 0; i < count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    close(output_pipe[1]);

    /* Read output while children run to avoid filling the pipe. */
    char buffer[4096];
    ssize_t bytes;

    for (;;) {
        bytes = read(output_pipe[0], buffer, sizeof(buffer));
        if (bytes > 0) {
            if (capture != NULL && capture_capacity > 0) {
                for (ssize_t i = 0; i < bytes; i++) {
                    if (captured_len + 1 < capture_capacity)
                        capture[captured_len++] = buffer[i];
                }
                capture[captured_len] = '\0';
            } else if (worker_context_slot >= 0) {
                worker_output_append(worker_context_slot, buffer, (size_t)bytes);
            } else {
                for (ssize_t i = 0; i < bytes; i++)
                    add_char(buffer[i]);
            }
            continue;
        }
        if (bytes == -1 && errno == EINTR)
            continue;
        break;
    }

    close(output_pipe[0]);

    int pipeline_status = 0;
    for (int i = 0; i < launched; i++) {
        int status = 0;
        while (waitpid(pids[i], &status, 0) == -1) {
            if (errno != EINTR)
                break;
        }
        if (i == launched - 1) {
            if (WIFEXITED(status))
                pipeline_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                pipeline_status = 128 + WTERMSIG(status);
            else
                pipeline_status = 1;
        }
    }

    last_exit_status = pipeline_status;
    if (worker_managed)
        worker_set_active_pgid(worker_context_slot, 0);
    if (capture != NULL && capture_capacity > 0)
        capture[captured_len] = '\0';
    return pipeline_status;
}

void execute_pipeline(ParsedCommand commands[], int count)
{
    (void)launch_interactive_pipeline(commands, count, requested_background,
                                     current_job_command);
    requested_background = 0;
}

void execute_command(char *command)
{
    requested_background = strip_background_operator(command);
    snprintf(current_job_command, sizeof(current_job_command), "%s%s",
             command, requested_background ? " &" : "");

    char watch_commands[MAX_WATCH_COMMANDS][INPUT_CAPACITY];
    int watch_count = 0;
    int watch_parse = parse_multiwatch_invocation(command, watch_commands, &watch_count);
    if (watch_parse != 0) {
        if (watch_parse < 0) {
            add_text("multiWatch: use multiWatch [\"command 1\", \"command 2\"]\n");
            last_exit_status = 2;
        } else if (requested_background) {
            add_text("multiWatch must run in the foreground; do not append &\n");
            last_exit_status = 2;
        } else {
            last_exit_status = run_multiwatch(watch_commands, watch_count);
        }
        requested_background = 0;
        return;
    }

    char expanded[MAX_COMMAND_SIZE];
    if (expand_shell_text(command, expanded, sizeof(expanded), 0) != 0) {
        add_text("terminator: expanded command is too long\n");
        last_exit_status = 2;
        requested_background = 0;
        return;
    }

    Token tokens[MAX_TOKENS];
    int token_count = 0;

    if (tokenize(expanded, tokens, &token_count) == -1)
        return;

    if (token_count == 0)
        return;

    ParsedCommand commands[MAX_COMMANDS];
    int command_count = 0;

    if (parse_command(tokens, token_count,
                      commands, &command_count) == -1)
        return;

    /* Basic built-ins run in the shell process. */
    if (!requested_background && command_count == 1 &&
        commands[0].infile == NULL &&
        commands[0].outfile == NULL) {
        char **argv = commands[0].argv;

        if (strcmp(argv[0], "exit") == 0) {
            last_exit_status = 0;
            if (tab_count > 1)
                close_active_tab();
            else
                running = 0;
            return;
        }

        if (strcmp(argv[0], "cd") == 0) {
            const char *path = argv[1] != NULL ? argv[1] : getenv("HOME");
            char resolved[PATH_MAX];
            if (resolve_tab_directory(path, resolved) != 0) {
                int saved_errno = errno;
                char message[MAX_LINE_LENGTH];
                snprintf(message, sizeof(message), "cd: %s: %s\n",
                         path != NULL ? path : "(home)", strerror(saved_errno));
                add_text(message);
                last_exit_status = 1;
            } else {
                set_tab_cwd(current_tab_slot(), resolved);
                last_exit_status = 0;
            }
            return;
        }

        if (strcmp(argv[0], "history") == 0) {
            show_history();
            last_exit_status = 0;
            return;
        }

        if (strcmp(argv[0], "clear") == 0) {
            line_count = 1;
            output[0][0] = '\0';
            scroll_offset = 0;
            last_exit_status = 0;
            return;
        }

        if (strcmp(argv[0], "jobs") == 0) {
            show_jobs();
            last_exit_status = 0;
            return;
        }

        if (strcmp(argv[0], "bg") == 0) {
            continue_job_in_background(argv[1]);
            return;
        }

        if (strcmp(argv[0], "fg") == 0) {
            bring_job_to_foreground(argv[1]);
            return;
        }
    }

    (void)launch_interactive_pipeline(commands, command_count,
                                      requested_background, current_job_command);
    requested_background = 0;
}
