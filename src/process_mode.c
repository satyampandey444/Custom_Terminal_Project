#include "terminator_internal.h"

/* Process Mode module. */

int process_write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = (const unsigned char *)data;
    while (length > 0) {
        ssize_t n = write(fd, p, length);
        if (n > 0) {
            p += (size_t)n;
            length -= (size_t)n;
        } else if (n == -1 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

int process_read_all(int fd, void *data, size_t length)
{
    unsigned char *p = (unsigned char *)data;
    size_t got = 0;
    while (got < length) {
        ssize_t n = read(fd, p + got, length - got);
        if (n > 0) {
            got += (size_t)n;
        } else if (n == 0) {
            return got == 0 ? 0 : -1;
        } else if (errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 1;
}

int process_send_frame(uint32_t type, const void *data, uint32_t length)
{
    if (!process_child_mode || process_output_fd < 0 || length > PROC_MAX_FRAME)
        return -1;
    ProcessMessageHeader header = {type, length};
    if (process_write_all(process_output_fd, &header, sizeof(header)) != 0)
        return -1;
    if (length > 0 && process_write_all(process_output_fd, data, length) != 0)
        return -1;
    return 0;
}

void process_show_shared_history_child(void)
{
    if (process_shared_history == NULL) {
        add_text("terminator: shared history is unavailable\n");
        return;
    }
    while (sem_wait(&process_shared_history->semaphore) == -1) {
        if (errno != EINTR) {
            add_text("terminator: could not lock shared history\n");
            return;
        }
    }
    int first = process_shared_history->count - HISTORY_DISPLAY_LIMIT;
    if (first < 0) first = 0;
    for (int i = first; i < process_shared_history->count; i++) {
        int index = (process_shared_history->start + i) % MAX_HISTORY;
        char line[1024];
        snprintf(line, sizeof(line), "%5d  %.900s\n", i + 1,
                 process_shared_history->entries[index]);
        add_text(line);
    }
    sem_post(&process_shared_history->semaphore);
}

void process_child_dispatch_command(int slot, char *command)
{
    char original[MAX_COMMAND_SIZE];
    char expanded[MAX_COMMAND_SIZE];
    char tokens_text[MAX_COMMAND_SIZE];
    int background;

    (void)slot;
    snprintf(original, sizeof(original), "%s", command);
    background = strip_background_operator(original);
    char *trimmed = original;
    while (isspace((unsigned char)*trimmed))
        trimmed++;
    size_t len = strlen(trimmed);
    while (len > 0 && isspace((unsigned char)trimmed[len - 1]))
        trimmed[--len] = '\0';
    if (*trimmed == '\0') {
        add_text("terminator: missing command before &\n");
        last_exit_status = 2;
        return;
    }
    snprintf(tokens_text, sizeof(tokens_text), "%s", trimmed);

    if (expand_shell_text(tokens_text, expanded, sizeof(expanded), 0) != 0) {
        add_text("terminator: expanded command is too long\n");
        last_exit_status = 2;
        return;
    }

    Token tokens[MAX_TOKENS];
    int token_count = 0;
    if (tokenize(expanded, tokens, &token_count) != 0) {
        last_exit_status = 2;
        return;
    }
    if (token_count == 0) {
        last_exit_status = 0;
        return;
    }

    ParsedCommand commands[MAX_COMMANDS];
    int command_count = 0;
    if (parse_command(tokens, token_count, commands, &command_count) != 0) {
        last_exit_status = 2;
        return;
    }

    if (!background && script_dispatch_if_requested(slot, commands, command_count))
        return;

    if (!background && command_count == 1 &&
        commands[0].infile == NULL && commands[0].outfile == NULL) {
        char **argv = commands[0].argv;
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
            int index = parse_job_id(argv[1]);
            if (index < 0) {
                add_text("fg: no such job\n");
                last_exit_status = 1;
                return;
            }
            if (jobs[index].state == JOB_STOPPED) {
                if (kill(-jobs[index].pgid, SIGCONT) == -1 && errno != ESRCH) {
                    add_text("fg: could not continue job\n");
                    last_exit_status = 1;
                    return;
                }
                for (int i = 0; i < jobs[index].proc_count; i++) {
                    if (jobs[index].proc_state[i] == 1)
                        jobs[index].proc_state[i] = 0;
                }
                jobs[index].state = JOB_RUNNING;
            }
            add_text(jobs[index].command);
            add_char('\n');
            (void)process_child_wait_foreground_job(index);
            return;
        }
        if (strcmp(argv[0], "cd") == 0 ||
            strcmp(argv[0], "history") == 0 ||
            strcmp(argv[0], "clear") == 0 ||
            strcmp(argv[0], "source") == 0) {
            /* Existing built-in/script path. */
            worker_execute_command(slot, command);
            return;
        }
    }

    (void)launch_interactive_pipeline(commands, command_count,
                                      background, trimmed);
}

int process_start(int slot)
{
    if (slot < 0 || slot >= MAX_TABS)
        return -1;
    if (process_tabs[slot].started)
        return 0;

    int command_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (pipe(command_pipe) == -1 || pipe(output_pipe) == -1) {
        if (command_pipe[0] >= 0) close(command_pipe[0]);
        if (command_pipe[1] >= 0) close(command_pipe[1]);
        if (output_pipe[0] >= 0) close(output_pipe[0]);
        if (output_pipe[1] >= 0) close(output_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(command_pipe[0]); close(command_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        (void)setpgid(0, 0);
        close(command_pipe[1]);
        close(output_pipe[0]);
        for (int i = 0; i < MAX_TABS; i++) {
            if (i == slot) continue;
            if (process_tabs[i].command_fd >= 0) close(process_tabs[i].command_fd);
            if (process_tabs[i].output_fd >= 0) close(process_tabs[i].output_fd);
        }
        if (display != NULL) {
            int inherited_xfd = ConnectionNumber(display);
            if (inherited_xfd >= 0) close(inherited_xfd);
        }
        display = NULL;
        process_child_mode = 1;
        process_output_fd = output_pipe[1];
        (void)fcntl(command_pipe[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(process_output_fd, F_SETFD, FD_CLOEXEC);
        worker_context_slot = slot;
        if (tab_states[slot].cwd[0] == '\0')
            (void)getcwd(tab_states[slot].cwd, sizeof(tab_states[slot].cwd));
        (void)process_send_frame(PROC_MSG_CWD, tab_states[slot].cwd,
                                 (uint32_t)(strlen(tab_states[slot].cwd) + 1));

        for (;;) {
            /* Keep background output flowing and reap stopped/completed jobs
               even when the user is not entering another command. */
            process_child_service_jobs(command_pipe[0]);
            if (process_child_mode == 0)
                break;
            /* select() above is time-bounded; now consume a command frame
               only when the parent has begun writing one. */
            fd_set command_ready;
            FD_ZERO(&command_ready);
            FD_SET(command_pipe[0], &command_ready);
            struct timeval command_timeout = {0, 0};
            int command_is_ready = select(command_pipe[0] + 1, &command_ready,
                                           NULL, NULL, &command_timeout);
            if (command_is_ready <= 0)
                continue;

            uint32_t length = 0;
            int read_result = process_read_all(command_pipe[0], &length, sizeof(length));
            if (read_result <= 0)
                break;
            if (length >= MAX_COMMAND_SIZE) {
                const char msg[] = "terminator: IPC command too long\n";
                (void)process_send_frame(PROC_MSG_OUTPUT, msg, (uint32_t)(sizeof(msg) - 1));
                break;
            }
            char command[MAX_COMMAND_SIZE];
            if (process_read_all(command_pipe[0], command, length) <= 0)
                break;
            command[length] = '\0';

            char trimmed[MAX_COMMAND_SIZE];
            snprintf(trimmed, sizeof(trimmed), "%s", command);
            char *start = trimmed;
            while (isspace((unsigned char)*start)) start++;
            char *end = start + strlen(start);
            while (end > start && isspace((unsigned char)end[-1])) *--end = '\0';
            if (strncmp(start, "exit", 4) == 0 &&
                (start[4] == '\0' || isspace((unsigned char)start[4]))) {
                (void)process_send_frame(PROC_MSG_EXIT, NULL, 0);
                break;
            }

            if (strcmp(start, "history") == 0) {
                process_show_shared_history_child();
                last_exit_status = 0;
            } else if (strcmp(start, "clear") == 0) {
                (void)process_send_frame(PROC_MSG_CLEAR, NULL, 0);
                last_exit_status = 0;
            } else if (strcmp(start, "jobs") == 0) {
                /* Handle jobs directly in the Process Mode shell loop. This
                   also avoids an occasional parser/executor fallback where
                   jobs was treated as an external executable. */
                show_jobs();
                last_exit_status = 0;
            } else if (strncmp(start, "multiWatch", 10) == 0 &&
                       (start[10] == '\0' || isspace((unsigned char)start[10]) || start[10] == '[')) {
                char watch_line[MAX_COMMAND_SIZE];
                snprintf(watch_line, sizeof(watch_line), "%s", start);
                if (strip_background_operator(watch_line)) {
                    add_text("multiWatch must run in the foreground; do not append &\n");
                    last_exit_status = 2;
                } else {
                    char watch_commands[MAX_WATCH_COMMANDS][INPUT_CAPACITY];
                    int watch_count = 0;
                    int parsed = parse_multiwatch_invocation(watch_line, watch_commands, &watch_count);
                    if (parsed <= 0) {
                        add_text("multiWatch: use multiWatch [\"command 1\", \"command 2\"]\n");
                        last_exit_status = 2;
                    } else {
                        (void)run_multiwatch_process_mode(slot, watch_commands, watch_count);
                    }
                }
            } else {
                process_child_dispatch_command(slot, command);
            }

            char cwd[PATH_MAX];
            snprintf(cwd, sizeof(cwd), "%s",
                     tab_states[slot].cwd[0] ? tab_states[slot].cwd : "/");
            (void)process_send_frame(PROC_MSG_CWD, cwd, (uint32_t)(strlen(cwd) + 1));
        }
        close(command_pipe[0]);
        close(output_pipe[1]);
        _exit(0);
    }

    close(command_pipe[0]);
    close(output_pipe[1]);
    int flags = fcntl(output_pipe[0], F_GETFL, 0);
    if (flags != -1)
        (void)fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
    process_tabs[slot].pid = pid;
    process_tabs[slot].command_fd = command_pipe[1];
    process_tabs[slot].output_fd = output_pipe[0];
    process_tabs[slot].started = 1;
    process_tabs[slot].active_pgid = 0;
    process_tabs[slot].incoming_length = 0;
    return 0;
}

int process_submit(int slot, const char *command)
{
    if (slot < 0 || slot >= MAX_TABS || command == NULL ||
        !process_tabs[slot].started)
        return -1;
    size_t length = strlen(command);
    if (length >= MAX_COMMAND_SIZE || length > UINT32_MAX)
        return -1;
    uint32_t size = (uint32_t)length;
    if (process_write_all(process_tabs[slot].command_fd, &size, sizeof(size)) != 0 ||
        process_write_all(process_tabs[slot].command_fd, command, length) != 0)
        return -1;
    return 0;
}

void close_tab_for_slot(int slot)
{
    if (slot < 0 || slot >= MAX_TABS || !slot_used[slot]) return;
    int visual = -1;
    for (int i = 0; i < tab_count; i++)
        if (tab_slots[i] == slot) { visual = i; break; }
    if (visual < 0) return;
    if (tab_count == 1) {
        process_stop(slot);
        running = 0;
        return;
    }
    int old_slot = current_tab_slot();
    save_tab_state(old_slot, 1);
    active_tab = visual;
    load_tab_state(slot);
    close_active_tab();
    int restore_visual = -1;
    for (int i = 0; i < tab_count; i++)
        if (tab_slots[i] == old_slot) { restore_visual = i; break; }
    if (restore_visual >= 0 && restore_visual != active_tab) {
        save_tab_state(current_tab_slot(), 1);
        active_tab = restore_visual;
        load_tab_state(current_tab_slot());
        draw_window();
    }
}

void process_stop(int slot)
{
    if (slot < 0 || slot >= MAX_TABS || !process_tabs[slot].started)
        return;
    ProcessTab *tab = &process_tabs[slot];
    if (tab->command_fd >= 0) {
        close(tab->command_fd);
        tab->command_fd = -1;
    }
    if (tab->active_pgid > 0)
        (void)kill(-tab->active_pgid, SIGKILL);
    if (tab->pid > 0) {
        (void)kill(tab->pid, SIGTERM);
        int status;
        while (waitpid(tab->pid, &status, 0) == -1 && errno == EINTR) {}
    }
    if (tab->output_fd >= 0) close(tab->output_fd);
    memset(tab, 0, sizeof(*tab));
    tab->command_fd = -1;
    tab->output_fd = -1;
}

void process_handle_message(int slot, const ProcessMessageHeader *header,
                                   const unsigned char *payload)
{
    if (header->type == PROC_MSG_OUTPUT) {
        char *text = malloc((size_t)header->length + 1);
        if (text == NULL) return;
        memcpy(text, payload, header->length);
        text[header->length] = '\0';
        append_text_to_tab(slot, text);
        free(text);
    } else if (header->type == PROC_MSG_CLEAR) {
        clear_output_for_slot(slot);
    } else if (header->type == PROC_MSG_CWD) {
        char cwd[PATH_MAX];
        size_t n = header->length < sizeof(cwd) ? header->length : sizeof(cwd) - 1;
        memcpy(cwd, payload, n);
        cwd[n] = '\0';
        if (cwd[0] == '/') set_tab_cwd(slot, cwd);
    } else if (header->type == PROC_MSG_PGID && header->length == sizeof(pid_t)) {
        memcpy(&process_tabs[slot].active_pgid, payload, sizeof(pid_t));
    } else if (header->type == PROC_MSG_EXIT) {
        close_tab_for_slot(slot);
    }
}

void process_drain_output(void)
{
    unsigned char temp[8192];
    for (int slot = 0; slot < MAX_TABS; slot++) {
        ProcessTab *tab = &process_tabs[slot];
        if (!tab->started || tab->output_fd < 0) continue;
        for (;;) {
            ssize_t n = read(tab->output_fd, temp, sizeof(temp));
            if (n > 0) {
                if ((size_t)n > sizeof(tab->incoming) - tab->incoming_length) {
                    tab->incoming_length = 0;
                    add_text("terminator: process IPC receive buffer overflow\n");
                }
                size_t available = sizeof(tab->incoming) - tab->incoming_length;
                size_t take = (size_t)n < available ? (size_t)n : available;
                memcpy(tab->incoming + tab->incoming_length, temp, take);
                tab->incoming_length += take;
                size_t consumed = 0;
                while (tab->incoming_length - consumed >= sizeof(ProcessMessageHeader)) {
                    ProcessMessageHeader header;
                    memcpy(&header, tab->incoming + consumed, sizeof(header));
                    if (header.length > PROC_MAX_FRAME) {
                        tab->incoming_length = 0;
                        consumed = 0;
                        add_text("terminator: invalid process IPC frame\n");
                        break;
                    }
                    size_t frame_size = sizeof(header) + (size_t)header.length;
                    if (tab->incoming_length - consumed < frame_size) break;
                    process_handle_message(slot, &header,
                                           tab->incoming + consumed + sizeof(header));
                    consumed += frame_size;
                    if (!tab->started) break;
                }
                if (consumed > 0 && tab->started) {
                    memmove(tab->incoming, tab->incoming + consumed,
                            tab->incoming_length - consumed);
                    tab->incoming_length -= consumed;
                }
                if (!tab->started) break;
                continue;
            }
            if (n == -1 && errno == EINTR) continue;
            if (n == 0) {
                close(tab->output_fd);
                tab->output_fd = -1;
                /* Keep the process record available for orderly wait/cleanup. */
                if (slot >= 0 && slot < MAX_TABS && slot_used[slot])
                    append_text_to_tab(slot, "\nterminator: tab process exited\n");
            }
            break; /* EAGAIN, EOF or error. */
        }
    }
}
