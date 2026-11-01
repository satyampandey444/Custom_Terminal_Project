#include "terminator_internal.h"

/* Workers module. */

int worker_start(int slot)
{
    if (use_process_mode)
        return process_start(slot);
    if (slot < 0 || slot >= MAX_TABS)
        return -1;
    TabWorker *worker = &tab_workers[slot];
    memset(worker, 0, sizeof(*worker));
    if (pthread_mutex_init(&worker->mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&worker->condition, NULL) != 0) {
        pthread_mutex_destroy(&worker->mutex);
        return -1;
    }
    worker->started = 1;
    int error = pthread_create(&worker->thread, NULL, tab_worker_main,
                               (void *)(intptr_t)slot);
    if (error != 0) {
        worker->started = 0;
        pthread_cond_destroy(&worker->condition);
        pthread_mutex_destroy(&worker->mutex);
        errno = error;
        return -1;
    }
    return 0;
}

int worker_submit(int slot, const char *command)
{
    if (use_process_mode)
        return process_submit(slot, command);
    if (slot < 0 || slot >= MAX_TABS || command == NULL ||
        !tab_workers[slot].started)
        return -1;
    TabWorker *worker = &tab_workers[slot];
    pthread_mutex_lock(&worker->mutex);
    if (worker->shutdown || worker->queue_count >= WORKER_COMMAND_QUEUE) {
        pthread_mutex_unlock(&worker->mutex);
        return -1;
    }
    int tail = (worker->queue_head + worker->queue_count) % WORKER_COMMAND_QUEUE;
    snprintf(worker->commands[tail], sizeof(worker->commands[tail]), "%s", command);
    worker->queue_count++;
    pthread_cond_signal(&worker->condition);
    pthread_mutex_unlock(&worker->mutex);
    return 0;
}

int worker_busy(int slot)
{
    if (use_process_mode)
        return 0; /* IPC commands are streamed; the GUI never waits for them. */
    if (slot < 0 || slot >= MAX_TABS || !tab_workers[slot].started)
        return 0;
    TabWorker *worker = &tab_workers[slot];
    pthread_mutex_lock(&worker->mutex);
    int busy = worker->busy || worker->queue_count > 0;
    pthread_mutex_unlock(&worker->mutex);
    return busy;
}

pid_t worker_active_pgid(int slot)
{
    if (use_process_mode) {
        if (slot < 0 || slot >= MAX_TABS || !process_tabs[slot].started) return 0;
        return process_tabs[slot].active_pgid;
    }
    if (slot < 0 || slot >= MAX_TABS || !tab_workers[slot].started)
        return 0;
    TabWorker *worker = &tab_workers[slot];
    pthread_mutex_lock(&worker->mutex);
    pid_t pgid = worker->active_pgid;
    pthread_mutex_unlock(&worker->mutex);
    return pgid;
}

void worker_set_active_pgid(int slot, pid_t pgid)
{
    if (process_child_mode) {
        (void)process_send_frame(PROC_MSG_PGID, &pgid, (uint32_t)sizeof(pgid));
        return;
    }
    if (use_process_mode) {
        if (slot >= 0 && slot < MAX_TABS && process_tabs[slot].started)
            process_tabs[slot].active_pgid = pgid;
        return;
    }
    if (slot < 0 || slot >= MAX_TABS || !tab_workers[slot].started)
        return;
    TabWorker *worker = &tab_workers[slot];
    pthread_mutex_lock(&worker->mutex);
    worker->active_pgid = pgid;
    pthread_mutex_unlock(&worker->mutex);
}

void worker_request_clear(int slot)
{
    if (process_child_mode) {
        (void)process_send_frame(PROC_MSG_CLEAR, NULL, 0);
        return;
    }
    if (slot < 0 || slot >= MAX_TABS || !tab_workers[slot].started)
        return;
    TabWorker *worker = &tab_workers[slot];
    pthread_mutex_lock(&worker->mutex);
    /* Discard output produced before clear, preserving command order. */
    worker->pending_length = 0;
    worker->pending_output[0] = '\0';
    worker->output_truncated = 0;
    worker->clear_requested = 1;
    pthread_mutex_unlock(&worker->mutex);
}

void worker_stop(int slot)
{
    if (use_process_mode) {
        process_stop(slot);
        return;
    }
    if (slot < 0 || slot >= MAX_TABS || !tab_workers[slot].started)
        return;
    TabWorker *worker = &tab_workers[slot];
    pthread_mutex_lock(&worker->mutex);
    worker->shutdown = 1;
    if (worker->active_pgid > 0)
        (void)kill(-worker->active_pgid, SIGKILL);
    worker->queue_count = 0;
    pthread_cond_broadcast(&worker->condition);
    pthread_mutex_unlock(&worker->mutex);
    (void)pthread_join(worker->thread, NULL);
    pthread_cond_destroy(&worker->condition);
    pthread_mutex_destroy(&worker->mutex);
    memset(worker, 0, sizeof(*worker));
}

void worker_drain_output(void)
{
    if (use_process_mode) {
        process_drain_output();
        return;
    }
    for (int slot = 0; slot < MAX_TABS; slot++) {
        TabWorker *worker = &tab_workers[slot];
        if (!worker->started || !slot_used[slot])
            continue;

        char local[16385];
        size_t take = 0;
        int clear_screen = 0;
        int truncated = 0;
        pthread_mutex_lock(&worker->mutex);
        clear_screen = worker->clear_requested;
        worker->clear_requested = 0;
        truncated = worker->output_truncated;
        worker->output_truncated = 0;
        if (worker->pending_length > 0) {
            take = worker->pending_length;
            if (take >= sizeof(local))
                take = sizeof(local) - 1;
            memcpy(local, worker->pending_output, take);
            local[take] = '\0';
            memmove(worker->pending_output, worker->pending_output + take,
                    worker->pending_length - take);
            worker->pending_length -= take;
            worker->pending_output[worker->pending_length] = '\0';
        }
        pthread_mutex_unlock(&worker->mutex);

        if (clear_screen) {
            int previous_slot = current_tab_slot();
            if (slot == previous_slot) {
                line_count = 1;
                output[0][0] = '\0';
                scroll_offset = 0;
            } else {
                tab_states[slot].line_count = 1;
                tab_states[slot].output[0][0] = '\0';
                tab_states[slot].scroll_offset = 0;
            }
        }
        if (truncated)
            append_text_to_tab(slot, "\n[terminator: worker output truncated]\n");
        if (take > 0)
            append_text_to_tab(slot, local);
    }
}

int should_run_on_gui_thread(const char *command)
{
    while (isspace((unsigned char)*command))
        command++;
    if (*command == '\0')
        return 1;
    char word[64];
    size_t n = 0;
    while (command[n] && !isspace((unsigned char)command[n]) &&
           command[n] != '|' && command[n] != '<' && command[n] != '>' &&
           command[n] != '&') {
        if (n + 1 < sizeof(word))
            word[n] = command[n];
        n++;
    }
    if (n >= sizeof(word))
        return 1;
    word[n] = '\0';
    if (strcmp(word, "exit") == 0 || strcmp(word, "multiWatch") == 0 ||
        strcmp(word, "jobs") == 0 || strcmp(word, "fg") == 0 ||
        strcmp(word, "bg") == 0)
        return 1;

    /* Background jobs require the existing job-table path. */
    char copy[MAX_COMMAND_SIZE];
    snprintf(copy, sizeof(copy), "%s", command);
    if (strip_background_operator(copy))
        return 1;
    return 0;
}

void worker_execute_command(int slot, char *command)
{
    worker_context_slot = slot;
    pthread_mutex_lock(&tab_data_mutex);
    last_exit_status = tab_states[slot].last_exit_status;
    pthread_mutex_unlock(&tab_data_mutex);

    char expanded[MAX_COMMAND_SIZE];
    if (expand_shell_text(command, expanded, sizeof(expanded), 0) != 0) {
        add_text("terminator: expanded command is too long\n");
        last_exit_status = 2;
        goto finished;
    }

    Token tokens[MAX_TOKENS];
    int token_count = 0;
    if (tokenize(expanded, tokens, &token_count) != 0) {
        last_exit_status = 2;
        goto finished;
    }
    if (token_count == 0) {
        last_exit_status = 0;
        goto finished;
    }

    ParsedCommand commands[MAX_COMMANDS];
    int command_count = 0;
    if (parse_command(tokens, token_count, commands, &command_count) != 0) {
        last_exit_status = 2;
        goto finished;
    }

    if (script_dispatch_if_requested(slot, commands, command_count))
        goto finished;

    if (command_count == 1 && commands[0].infile == NULL &&
        commands[0].outfile == NULL) {
        char **argv = commands[0].argv;
        if (strcmp(argv[0], "cd") == 0) {
            const char *target = argv[1] != NULL ? argv[1] : getenv("HOME");
            char resolved[PATH_MAX];
            if (resolve_tab_directory(target, resolved) != 0) {
                char message[PATH_MAX + 256];
                int saved_errno = errno;
                snprintf(message, sizeof(message), "cd: %s: %s\n",
                         target != NULL ? target : "(home)", strerror(saved_errno));
                add_text(message);
                last_exit_status = 1;
            } else {
                set_tab_cwd(slot, resolved);
                if (process_child_mode)
                    (void)setenv("PWD", resolved, 1);
                last_exit_status = 0;
            }
            goto finished;
        }
        if (strcmp(argv[0], "history") == 0) {
            if (process_child_mode)
                process_show_shared_history_child();
            else
                show_history();
            last_exit_status = 0;
            goto finished;
        }
        if (strcmp(argv[0], "clear") == 0) {
            worker_request_clear(slot);
            last_exit_status = 0;
            goto finished;
        }
    }

    (void)execute_pipeline_internal(commands, command_count, NULL, 0);

finished:
    pthread_mutex_lock(&tab_data_mutex);
    tab_states[slot].last_exit_status = last_exit_status;
    pthread_mutex_unlock(&tab_data_mutex);
    worker_context_slot = -1;
}

void dispatch_command(char *command)
{
    int slot = current_tab_slot();
    if (use_process_mode) {
        if (process_submit(slot, command) != 0)
            add_text("terminator: could not send command to tab process\n");
        return;
    }
    if (should_run_on_gui_thread(command)) {
        /* Maintain per-tab $? for legacy GUI-thread built-ins and job control. */
        while (worker_busy(slot)) {
            worker_drain_output();
            struct timespec pause = {0, 10000000};
            nanosleep(&pause, NULL);
        }
        pthread_mutex_lock(&tab_data_mutex);
        last_exit_status = tab_states[slot].last_exit_status;
        pthread_mutex_unlock(&tab_data_mutex);
        execute_command(command);
        if (slot_used[slot]) {
            pthread_mutex_lock(&tab_data_mutex);
            tab_states[slot].last_exit_status = last_exit_status;
            pthread_mutex_unlock(&tab_data_mutex);
        }
    } else if (worker_submit(slot, command) != 0) {
        add_text("terminator: tab worker queue is full or unavailable\n");
    }
}
