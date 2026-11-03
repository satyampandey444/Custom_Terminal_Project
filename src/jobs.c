#include "terminator_internal.h"

/* Jobs module. */

int reserve_job(const char *command, int foreground)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active) {
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].active = 1;
            jobs[i].id = next_job_id++;
            jobs[i].owner_tab = current_tab_slot();
            jobs[i].output_fd = -1;
            jobs[i].state = JOB_RUNNING;
            jobs[i].is_foreground = foreground;
            snprintf(jobs[i].command, sizeof(jobs[i].command), "%s",
                     command != NULL ? command : "");
            return i;
        }
    }
    return -1;
}

void release_job(int index)
{
    if (index < 0 || index >= MAX_JOBS)
        return;
    if (jobs[index].output_fd >= 0)
        close(jobs[index].output_fd);
    memset(&jobs[index], 0, sizeof(jobs[index]));
    jobs[index].output_fd = -1;
}

int job_refresh_status(Job *job)
{
    int alive = 0;
    int stopped = 0;

    for (int i = 0; i < job->proc_count; i++) {
        if (job->proc_state[i] == 2)
            continue;

        int status = 0;
        pid_t result;
        do {
            result = waitpid(job->pids[i], &status,
                             WNOHANG | WUNTRACED | WCONTINUED);
        } while (result == -1 && errno == EINTR);

        if (result == job->pids[i]) {
            if (WIFSTOPPED(status)) {
                job->proc_state[i] = 1;
            } else if (WIFCONTINUED(status)) {
                job->proc_state[i] = 0;
            } else if (WIFEXITED(status)) {
                job->proc_state[i] = 2;
                if (i == job->proc_count - 1)
                    job->last_status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                job->proc_state[i] = 2;
                if (i == job->proc_count - 1)
                    job->last_status = 128 + WTERMSIG(status);
            }
        } else if (result == -1 && errno == ECHILD) {
            job->proc_state[i] = 2;
        }

        if (job->proc_state[i] != 2) {
            alive++;
            if (job->proc_state[i] == 1)
                stopped++;
        }
    }

    if (alive == 0)
        job->state = JOB_DONE;
    else if (stopped == alive)
        job->state = JOB_STOPPED;
    else
        job->state = JOB_RUNNING;

    return job->state;
}

void read_job_output(Job *job)
{
    if (job == NULL || job->output_fd < 0)
        return;

    char buffer[4096];
    ssize_t n = read(job->output_fd, buffer, sizeof(buffer));
    if (n > 0) {
        char text[4097];
        size_t used = (size_t)n < sizeof(text) - 1 ? (size_t)n : sizeof(text) - 1;
        memcpy(text, buffer, used);
        text[used] = '\0';
        append_text_to_tab(job->owner_tab, text);
        if (!process_child_mode && job->owner_tab == current_tab_slot())
            draw_window();
    } else if (n == 0) {
        close(job->output_fd);
        job->output_fd = -1;
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        close(job->output_fd);
        job->output_fd = -1;
    }
}

void add_background_job_fds(fd_set *readfds, int *maxfd,
                                   int skip_index)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (i == skip_index || !jobs[i].active || jobs[i].output_fd < 0)
            continue;
        FD_SET(jobs[i].output_fd, readfds);
        if (jobs[i].output_fd > *maxfd)
            *maxfd = jobs[i].output_fd;
    }
}

void service_background_job_fds(fd_set *readfds, int skip_index,
                                       const int *snapshot_fds)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (i == skip_index || !jobs[i].active)
            continue;
        int fd = snapshot_fds != NULL ? snapshot_fds[i] : jobs[i].output_fd;
        if (fd >= 0 && jobs[i].output_fd == fd && FD_ISSET(fd, readfds))
            read_job_output(&jobs[i]);
    }
}

void refresh_background_jobs(int skip_index)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (i == skip_index || !jobs[i].active)
            continue;

        int old_state = jobs[i].state;
        int state = job_refresh_status(&jobs[i]);
        if (!jobs[i].is_foreground && old_state != state) {
            char message[INPUT_CAPACITY + 128];
            if (state == JOB_STOPPED) {
                snprintf(message, sizeof(message), "[%d]+ Stopped  %s\n",
                         jobs[i].id, jobs[i].command);
                append_text_to_tab(jobs[i].owner_tab, message);
            } else if (state == JOB_DONE && !jobs[i].completion_notified) {
                snprintf(message, sizeof(message), "[%d]+ Done     %s\n",
                         jobs[i].id, jobs[i].command);
                append_text_to_tab(jobs[i].owner_tab, message);
                jobs[i].completion_notified = 1;
            }
        }
        if (state == JOB_DONE && jobs[i].output_fd < 0)
            release_job(i);
    }
}

void handle_waiting_x_events(int job_index)
{
    while (XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == Expose) {
            draw_window();
        } else if (event.type == ButtonPress) {
            handle_tab_click(event.xbutton.x, event.xbutton.y);
        } else if (event.type == KeyPress) {
            char chars[32];
            KeySym key;
            XLookupString(&event.xkey, chars, sizeof(chars), &key, NULL);
            if (handle_tab_hotkey(key, event.xkey.state))
                continue;
            int control = (event.xkey.state & ControlMask) != 0;
            if (job_index >= 0 && job_index < MAX_JOBS && jobs[job_index].active &&
                jobs[job_index].owner_tab == current_tab_slot() && control &&
                (key == XK_c || key == XK_C)) {
                if (jobs[job_index].state != JOB_DONE) {
                    (void)kill(-jobs[job_index].pgid, SIGINT);
                    add_text("^C\n");
                    draw_window();
                }
            } else if (job_index >= 0 && job_index < MAX_JOBS && jobs[job_index].active &&
                       jobs[job_index].owner_tab == current_tab_slot() && control &&
                       (key == XK_z || key == XK_Z)) {
                if (jobs[job_index].state != JOB_DONE) {
                    (void)kill(-jobs[job_index].pgid, SIGTSTP);
                    add_text("^Z\n");
                    draw_window();
                }
            }
        }
    }
}

void wait_foreground_job(int index)
{
    if (index < 0 || index >= MAX_JOBS || !jobs[index].active)
        return;

    jobs[index].is_foreground = 1;
    for (;;) {
        int state = job_refresh_status(&jobs[index]);
        if (state == JOB_STOPPED) {
            jobs[index].is_foreground = 0;
            char message[INPUT_CAPACITY + 128];
            snprintf(message, sizeof(message), "[%d]+ Stopped  %s\n",
                     jobs[index].id, jobs[index].command);
            add_text(message);
            draw_window();
            return;
        }
        if (state == JOB_DONE && jobs[index].output_fd < 0) {
            int owner = jobs[index].owner_tab;
            int status = jobs[index].last_status;
            if (owner == current_tab_slot())
                last_exit_status = status;
            else if (owner >= 0 && owner < tab_count)
                tab_states[owner].last_exit_status = status;
            release_job(index);
            return;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        int xfd = ConnectionNumber(display);
        FD_SET(xfd, &readfds);
        int maxfd = xfd;
        int own_fd = jobs[index].output_fd;
        if (own_fd >= 0) {
            FD_SET(own_fd, &readfds);
            if (own_fd > maxfd)
                maxfd = own_fd;
        }
        int snapshots[MAX_JOBS];
        for (int i = 0; i < MAX_JOBS; i++)
            snapshots[i] = jobs[i].active ? jobs[i].output_fd : -1;
        add_background_job_fds(&readfds, &maxfd, index);
        struct timeval timeout = {0, 200000};
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0 && errno != EINTR) {
            add_text("terminator: select failed while waiting for job\n");
            (void)kill(-jobs[index].pgid, SIGTERM);
        }
        if (ready > 0 && FD_ISSET(xfd, &readfds))
            handle_waiting_x_events(index);
        if (own_fd >= 0 && jobs[index].active &&
            jobs[index].output_fd == own_fd && ready > 0 &&
            FD_ISSET(own_fd, &readfds))
            read_job_output(&jobs[index]);
        if (ready > 0)
            service_background_job_fds(&readfds, index, snapshots);
        refresh_background_jobs(index);
        draw_window();
    }
}

int launch_interactive_pipeline(ParsedCommand commands[], int count,
                                       int background, const char *command_line)
{
    int pipes[MAX_COMMANDS - 1][2];
    int output_pipe[2];
    pid_t local_pids[MAX_COMMANDS];
    int launched = 0;
    pid_t pgid = 0;
    int job_index = -1;

    if (count < 1 || count > MAX_COMMANDS)
        return 1;

    for (int i = 0; i < count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            add_text("terminator: pipe failed\n");
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return 1;
        }
    }
    if (pipe(output_pipe) == -1) {
        add_text("terminator: output pipe failed\n");
        for (int i = 0; i < count - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        return 1;
    }

    job_index = reserve_job(command_line, !background);
    if (job_index < 0) {
        add_text("terminator: job table is full\n");
        for (int i = 0; i < count - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        close(output_pipe[0]);
        close(output_pipe[1]);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            add_text("terminator: fork failed\n");
            break;
        }
        if (pid == 0) {
            if (enter_active_tab_directory() != 0)
                _exit(126);
            if (pgid == 0)
                (void)setpgid(0, 0);
            else
                (void)setpgid(0, pgid);

            (void)signal(SIGINT, SIG_DFL);
            (void)signal(SIGTSTP, SIG_DFL);
            (void)signal(SIGQUIT, SIG_DFL);
            (void)signal(SIGPIPE, SIG_DFL);

            if (dup2(output_pipe[1], STDERR_FILENO) == -1)
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
                out_fd = output_pipe[1];
            }

            if (dup2(in_fd, STDIN_FILENO) == -1 ||
                dup2(out_fd, STDOUT_FILENO) == -1) {
                dprintf(STDERR_FILENO, "terminator: dup2 failed: %s\n",
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
            dprintf(STDERR_FILENO, "terminator: %s: command not found\n",
                    commands[i].argv[0]);
            _exit(127);
        }

        if (pgid == 0)
            pgid = pid;
        (void)setpgid(pid, pgid);
        local_pids[launched++] = pid;
    }

    for (int i = 0; i < count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    close(output_pipe[1]);

    if (launched == 0) {
        close(output_pipe[0]);
        release_job(job_index);
        return 1;
    }

    jobs[job_index].pgid = pgid;
    jobs[job_index].proc_count = launched;
    jobs[job_index].output_fd = output_pipe[0];
    jobs[job_index].last_status = 0;
    for (int i = 0; i < launched; i++) {
        jobs[job_index].pids[i] = local_pids[i];
        jobs[job_index].proc_state[i] = 0;
    }

    if (background) {
        jobs[job_index].is_foreground = 0;
        char message[128];
        snprintf(message, sizeof(message), "[%d] %ld\n",
                 jobs[job_index].id, (long)pgid);
        add_text(message);
        last_exit_status = 0;
        return 0;
    }

    int owner_tab = jobs[job_index].owner_tab;
    if (process_child_mode) {
        worker_set_active_pgid(owner_tab, pgid);
        return process_child_wait_foreground_job(job_index);
    }

    wait_foreground_job(job_index);
    if (owner_tab >= 0 && owner_tab < tab_count) {
        if (owner_tab == current_tab_slot())
            return last_exit_status;
        return tab_states[owner_tab].last_exit_status;
    }
    return last_exit_status;
}

int parse_job_id(const char *spec)
{
    if (spec == NULL || *spec == '\0') {
        int newest = -1;
        int newest_id = -1;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].active && jobs[i].owner_tab == current_tab_slot() &&
                jobs[i].id > newest_id && jobs[i].state != JOB_DONE) {
                newest_id = jobs[i].id;
                newest = i;
            }
        }
        return newest;
    }

    if (*spec == '%')
        spec++;
    char *end = NULL;
    errno = 0;
    long id = strtol(spec, &end, 10);
    if (errno != 0 || end == spec || *end != '\0' || id <= 0)
        return -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].owner_tab == current_tab_slot() &&
            jobs[i].id == id && jobs[i].state != JOB_DONE)
            return i;
    }
    return -1;
}

void show_jobs(void)
{
    refresh_background_jobs(-1);
    int found = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active || jobs[i].owner_tab != current_tab_slot())
            continue;
        const char *state = jobs[i].state == JOB_STOPPED ? "Stopped" :
                            jobs[i].state == JOB_DONE ? "Done" : "Running";
        char message[INPUT_CAPACITY + 128];
        snprintf(message, sizeof(message), "[%d] %-7s %s\n",
                 jobs[i].id, state, jobs[i].command);
        add_text(message);
        found = 1;
    }
    if (!found)
        add_text("No active jobs\n");
}

void continue_job_in_background(const char *spec)
{
    int index = parse_job_id(spec);
    if (index < 0) {
        add_text("bg: no such job\n");
        last_exit_status = 1;
        return;
    }
    if (kill(-jobs[index].pgid, SIGCONT) == -1 && errno != ESRCH) {
        add_text("bg: could not continue job\n");
        last_exit_status = 1;
        return;
    }
    for (int i = 0; i < jobs[index].proc_count; i++) {
        if (jobs[index].proc_state[i] == 1)
            jobs[index].proc_state[i] = 0;
    }
    jobs[index].state = JOB_RUNNING;
    jobs[index].is_foreground = 0;
    char message[INPUT_CAPACITY + 128];
    snprintf(message, sizeof(message), "[%d]+ %s &\n",
             jobs[index].id, jobs[index].command);
    add_text(message);
    last_exit_status = 0;
}

void bring_job_to_foreground(const char *spec)
{
    int index = parse_job_id(spec);
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
    wait_foreground_job(index);
}

int strip_background_operator(char *command)
{
    size_t len = strlen(command);
    while (len > 0 && (command[len - 1] == ' ' || command[len - 1] == '\t' ||
                       command[len - 1] == '\n'))
        len--;
    if (len == 0 || command[len - 1] != '&')
        return 0;

    int quote = 0;
    int escaped = 0;
    for (size_t i = 0; i < len - 1; i++) {
        char c = command[i];
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (c == '\\' && quote != '\'') {
            escaped = 1;
            continue;
        }
        if (quote == 0) {
            if (c == '\'' || c == '"' || c == '`')
                quote = (unsigned char)c;
        } else if (c == quote) {
            quote = 0;
        }
    }
    if (quote != 0 || escaped)
        return 0;

    command[len - 1] = '\0';
    while (len > 1 && (command[len - 2] == ' ' || command[len - 2] == '\t'))
        command[--len - 1] = '\0';
    return 1;
}

int process_child_wait_foreground_job(int index)
{
    if (index < 0 || index >= MAX_JOBS || !jobs[index].active)
        return 1;

    Job *job = &jobs[index];
    int slot = job->owner_tab;
    job->is_foreground = 1;
    if (job->output_fd >= 0) {
        int flags = fcntl(job->output_fd, F_GETFL, 0);
        if (flags >= 0)
            (void)fcntl(job->output_fd, F_SETFL, flags | O_NONBLOCK);
    }
    worker_set_active_pgid(slot, job->pgid);

    for (;;) {
        /* Drain all currently available output without blocking the shell. */
        if (job->output_fd >= 0) {
            char buffer[4096];
            for (;;) {
                ssize_t n = read(job->output_fd, buffer, sizeof(buffer));
                if (n > 0) {
                    char text[4097];
                    size_t used = (size_t)n;
                    memcpy(text, buffer, used);
                    text[used] = '\0';
                    add_text(text);
                    continue;
                }
                if (n == 0) {
                    close(job->output_fd);
                    job->output_fd = -1;
                } else if (errno != EINTR && errno != EAGAIN &&
                           errno != EWOULDBLOCK) {
                    close(job->output_fd);
                    job->output_fd = -1;
                }
                if (n == -1 && errno == EINTR)
                    continue;
                break;
            }
        }

        int state = job_refresh_status(job);
        if (state == JOB_STOPPED) {
            job->is_foreground = 0;
            char message[INPUT_CAPACITY + 128];
            snprintf(message, sizeof(message), "[%d]+ Stopped  %s\n",
                     job->id, job->command);
            add_text(message);
            worker_set_active_pgid(slot, 0);
            last_exit_status = 128 + SIGTSTP;
            return last_exit_status;
        }

        if (state == JOB_DONE) {
            /* All writers have exited; drain trailing output then release. */
            if (job->output_fd >= 0) {
                char buffer[4096];
                for (;;) {
                    ssize_t n = read(job->output_fd, buffer, sizeof(buffer));
                    if (n > 0) {
                        char text[4097];
                        size_t used = (size_t)n;
                        memcpy(text, buffer, used);
                        text[used] = '\0';
                        add_text(text);
                    } else {
                        if (n == 0 || (n < 0 && errno != EAGAIN &&
                                       errno != EWOULDBLOCK && errno != EINTR)) {
                            close(job->output_fd);
                            job->output_fd = -1;
                        }
                        break;
                    }
                }
            }
            int status = job->last_status;
            worker_set_active_pgid(slot, 0);
            last_exit_status = status;
            if (job->output_fd < 0)
                release_job(index);
            else {
                close(job->output_fd);
                job->output_fd = -1;
                release_job(index);
            }
            return last_exit_status;
        }

        /* Wait briefly for output; also periodically check child stop/exit. */
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;
        if (job->output_fd >= 0) {
            FD_SET(job->output_fd, &readfds);
            maxfd = job->output_fd;
        }
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        int selected = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (selected < 0 && errno != EINTR) {
            add_text("terminator: select failed while waiting for process job\n");
            (void)kill(-job->pgid, SIGTERM);
        }
    }
}

void process_child_service_jobs(int command_fd)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(command_fd, &readfds);
    int maxfd = command_fd;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].owner_tab == current_tab_slot() &&
            jobs[i].output_fd >= 0) {
            FD_SET(jobs[i].output_fd, &readfds);
            if (jobs[i].output_fd > maxfd)
                maxfd = jobs[i].output_fd;
        }
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
    if (ready < 0 && errno != EINTR)
        return;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active || jobs[i].owner_tab != current_tab_slot() ||
            jobs[i].output_fd < 0)
            continue;
        if (ready > 0 && FD_ISSET(jobs[i].output_fd, &readfds))
            read_job_output(&jobs[i]);
    }
    refresh_background_jobs(-1);
}
