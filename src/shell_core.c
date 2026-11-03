#include "terminator_internal.h"

/* Shell Core module. */

SuffixState *build_suffix_automaton(const char *query,
                                           int query_length,
                                           int *state_count_out)
{
    if (state_count_out != NULL)
        *state_count_out = 0;
    if (query == NULL || query_length <= 0)
        return NULL;

    size_t capacity = (size_t)query_length * 2U + 1U;
    SuffixState *states = calloc(capacity, sizeof(*states));
    if (states == NULL)
        return NULL;

    suffix_state_init(&states[0]);
    int state_count = 1;
    int last = 0;

    for (int i = 0; i < query_length; i++) {
        unsigned char ch = (unsigned char)query[i];
        if (ch >= SAM_ALPHABET) {
            free(states);
            return NULL;
        }

        int current = state_count++;
        suffix_state_init(&states[current]);
        states[current].length = states[last].length + 1;

        int pstate = last;
        while (pstate != -1 && states[pstate].next[ch] == -1) {
            states[pstate].next[ch] = current;
            pstate = states[pstate].link;
        }

        if (pstate == -1) {
            states[current].link = 0;
        } else {
            int qstate = states[pstate].next[ch];
            if (states[pstate].length + 1 == states[qstate].length) {
                states[current].link = qstate;
            } else {
                int clone = state_count++;
                states[clone] = states[qstate];
                states[clone].length = states[pstate].length + 1;

                while (pstate != -1 && states[pstate].next[ch] == qstate) {
                    states[pstate].next[ch] = clone;
                    pstate = states[pstate].link;
                }

                states[qstate].link = clone;
                states[current].link = clone;
            }
        }
        last = current;
    }

    if (state_count_out != NULL)
        *state_count_out = state_count;
    return states;
}

const char *find_substitution_end(const char *source)
{
    int depth = 1;
    int quote = 0;
    int backtick = 0;

    for (const char *p = source; *p; p++) {
        char c = *p;

        if (quote == '\'') {
            if (c == '\'')
                quote = 0;
            continue;
        }

        if (c == '\\') {
            if (p[1] != '\0')
                p++;
            continue;
        }

        if (quote == '"') {
            if (c == '"') {
                quote = 0;
                continue;
            }
            if (c == '$' && p[1] == '(') {
                depth++;
                p++;
                continue;
            }
            if (c == '(') {
                depth++;
                continue;
            }
            if (c == ')') {
                depth--;
                if (depth == 0)
                    return p;
            }
            continue;
        }

        if (backtick) {
            if (c == '`')
                backtick = 0;
            continue;
        }

        if (c == '\'') {
            quote = '\'';
            continue;
        }
        if (c == '"') {
            quote = '"';
            continue;
        }
        if (c == '`') {
            backtick = 1;
            continue;
        }
        if (c == '$' && p[1] == '(') {
            depth++;
            p++;
            continue;
        }
        if (c == '(') {
            depth++;
            continue;
        }
        if (c == ')') {
            depth--;
            if (depth == 0)
                return p;
        }
    }
    return NULL;
}

const char *find_backtick_end(const char *source)
{
    for (const char *p = source; *p; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            continue;
        }
        if (*p == '`')
            return p;
    }
    return NULL;
}

void *tab_worker_main(void *argument)
{
    int slot = (int)(intptr_t)argument;
    TabWorker *worker = &tab_workers[slot];
    for (;;) {
        char command[MAX_COMMAND_SIZE];
        pthread_mutex_lock(&worker->mutex);
        while (!worker->shutdown && worker->queue_count == 0)
            pthread_cond_wait(&worker->condition, &worker->mutex);
        if (worker->shutdown) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }
        snprintf(command, sizeof(command), "%s", worker->commands[worker->queue_head]);
        worker->queue_head = (worker->queue_head + 1) % WORKER_COMMAND_QUEUE;
        worker->queue_count--;
        worker->busy = 1;
        pthread_mutex_unlock(&worker->mutex);

        worker_execute_command(slot, command);

        pthread_mutex_lock(&worker->mutex);
        worker->busy = 0;
        pthread_cond_broadcast(&worker->condition);
        pthread_mutex_unlock(&worker->mutex);
    }
    return NULL;
}
