#include "terminator_internal.h"

/* History module. */

const char *history_at(int logical_index)
{
    if (logical_index < 0 || logical_index >= history_count)
        return "";
    return history_entries[(history_start + logical_index) % MAX_HISTORY];
}

int process_shared_history_init(void)
{
    process_shared_history = mmap(NULL, sizeof(*process_shared_history),
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (process_shared_history == MAP_FAILED) {
        process_shared_history = NULL;
        return -1;
    }
    memset(process_shared_history, 0, sizeof(*process_shared_history));
    if (sem_init(&process_shared_history->semaphore, 1, 1) == -1) {
        munmap(process_shared_history, sizeof(*process_shared_history));
        process_shared_history = NULL;
        return -1;
    }
    return 0;
}

void process_shared_history_append(const char *command)
{
    if (process_shared_history == NULL || command == NULL ||
        is_blank_command(command))
        return;
    while (sem_wait(&process_shared_history->semaphore) == -1) {
        if (errno == EINTR)
            continue;
        return;
    }
    int index;
    if (process_shared_history->count < MAX_HISTORY) {
        index = (process_shared_history->start + process_shared_history->count) % MAX_HISTORY;
        process_shared_history->count++;
    } else {
        index = process_shared_history->start;
        process_shared_history->start = (process_shared_history->start + 1) % MAX_HISTORY;
    }
    snprintf(process_shared_history->entries[index], INPUT_CAPACITY, "%s", command);
    sem_post(&process_shared_history->semaphore);
}

void history_store_memory(const char *command)
{
    /* The fixed-size shared ring is inherited across fork() in Process Mode. */
    process_shared_history_append(command);
    size_t length = strlen(command);
    char *copy = malloc(length + 1);
    if (copy == NULL)
        return;
    memcpy(copy, command, length + 1);

    if (history_count < MAX_HISTORY) {
        int slot = (history_start + history_count) % MAX_HISTORY;
        history_entries[slot] = copy;
        history_count++;
    } else {
        free(history_entries[history_start]);
        history_entries[history_start] = copy;
        history_start = (history_start + 1) % MAX_HISTORY;
    }
}

int history_write_encoded(FILE *file, const char *command)
{
    /* A prefix identifies escaped entries and keeps legacy history readable. */
    if (fputs("@T1:", file) == EOF)
        return -1;
    for (const unsigned char *p = (const unsigned char *)command; *p; p++) {
        if (*p == '\\') {
            if (fputs("\\\\", file) == EOF)
                return -1;
        } else if (*p == '\n') {
            if (fputs("\\n", file) == EOF)
                return -1;
        } else if (*p == '\r') {
            if (fputs("\\r", file) == EOF)
                return -1;
        } else if (fputc(*p, file) == EOF) {
            return -1;
        }
    }
    return fputc('\n', file) == EOF ? -1 : 0;
}

void history_decode_encoded(char *line)
{
    size_t read_pos = 0, write_pos = 0;
    while (line[read_pos] != '\0') {
        if (line[read_pos] == '\\' && line[read_pos + 1] != '\0') {
            char next = line[read_pos + 1];
            if (next == 'n') {
                line[write_pos++] = '\n';
                read_pos += 2;
                continue;
            }
            if (next == 'r') {
                line[write_pos++] = '\r';
                read_pos += 2;
                continue;
            }
            if (next == '\\') {
                line[write_pos++] = '\\';
                read_pos += 2;
                continue;
            }
        }
        line[write_pos++] = line[read_pos++];
    }
    line[write_pos] = '\0';
}

int history_rewrite_file(void)
{
    if (history_path[0] == '\0')
        return -1;
    FILE *file = fopen(history_path, "w");
    if (file == NULL)
        return -1;
    for (int i = 0; i < history_count; i++) {
        if (history_write_encoded(file, history_at(i)) != 0) {
            fclose(file);
            return -1;
        }
    }
    return fclose(file);
}

void history_load(void)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        home = ".";
    if (snprintf(history_path, sizeof(history_path), "%s/.terminator_history", home)
        >= (int)sizeof(history_path)) {
        history_path[0] = '\0';
        return;
    }

    FILE *file = fopen(history_path, "r");
    if (file == NULL)
        return; /* A missing history file is normal on first launch. */

    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int dropped_old_entries = 0;
    while ((length = getline(&line, &capacity, file)) != -1) {
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (length == 0)
            continue;
        if (strncmp(line, "@T1:", 4) == 0) {
            memmove(line, line + 4, strlen(line + 4) + 1);
            history_decode_encoded(line);
        }
        if (strlen(line) >= INPUT_CAPACITY)
            line[INPUT_CAPACITY - 1] = '\0';
        if (history_count == MAX_HISTORY)
            dropped_old_entries = 1;
        history_store_memory(line);
    }
    free(line);
    fclose(file);

    if (dropped_old_entries)
        (void)history_rewrite_file();
}

int is_blank_command(const char *command)
{
    while (*command != '\0') {
        if (!isspace((unsigned char)*command))
            return 0;
        command++;
    }
    return 1;
}

void history_record_unlocked(const char *command)
{
    if (is_blank_command(command))
        return;

    int was_full = (history_count == MAX_HISTORY);
    history_store_memory(command);

    if (history_path[0] == '\0')
        return;

    if (was_full) {
        if (history_rewrite_file() != 0)
            add_text("terminator: could not update history file\n");
    } else {
        FILE *file = fopen(history_path, "a");
        if (file == NULL) {
            add_text("terminator: could not open history file\n");
        } else {
            if (history_write_encoded(file, command) != 0)
                add_text("terminator: could not write history file\n");
            fclose(file);
        }
    }
}

void history_record(const char *command)
{
    pthread_mutex_lock(&history_mutex);
    history_record_unlocked(command);
    pthread_mutex_unlock(&history_mutex);
}

void show_history_unlocked(void)
{
    int first = history_count - HISTORY_DISPLAY_LIMIT;
    if (first < 0)
        first = 0;

    for (int i = first; i < history_count; i++) {
        char line[MAX_LINE_LENGTH];
        snprintf(line, sizeof(line), "%5d  %.900s\n", i + 1, history_at(i));
        add_text(line);
    }
}

void show_history(void)
{
    pthread_mutex_lock(&history_mutex);
    show_history_unlocked();
    pthread_mutex_unlock(&history_mutex);
}

void load_history_line(int logical_index)
{
    const char *saved = history_at(logical_index);
    snprintf(input, sizeof(input), "%s", saved);
    input_len = strlen(input);
    input_cursor = input_len;
}

void history_up(void)
{
    pthread_mutex_lock(&history_mutex);
    if (history_count != 0) {
        if (history_navigation == -1) {
            snprintf(history_saved_input, sizeof(history_saved_input), "%s", input);
            history_navigation = history_count - 1;
        } else if (history_navigation > 0) {
            history_navigation--;
        }
        load_history_line(history_navigation);
    }
    pthread_mutex_unlock(&history_mutex);
}

void history_down(void)
{
    pthread_mutex_lock(&history_mutex);
    if (history_navigation != -1) {
        if (history_navigation < history_count - 1) {
            history_navigation++;
            load_history_line(history_navigation);
        } else {
            history_navigation = -1;
            snprintf(input, sizeof(input), "%s", history_saved_input);
            input_len = strlen(input);
            input_cursor = input_len;
        }
    }
    pthread_mutex_unlock(&history_mutex);
}

void suffix_state_init(SuffixState *state)
{
    state->link = -1;
    state->length = 0;
    for (int c = 0; c < SAM_ALPHABET; c++)
        state->next[c] = -1;
}

int suffix_automaton_match(const SuffixState *states,
                                  const char *text)
{
    if (states == NULL || text == NULL)
        return 0;

    int state = 0;
    int current_length = 0;
    int best = 0;

    for (const unsigned char *ptr = (const unsigned char *)text; *ptr; ptr++) {
        unsigned char ch = *ptr;
        if (ch >= SAM_ALPHABET) {
            state = 0;
            current_length = 0;
            continue;
        }

        while (state != 0 && states[state].next[ch] == -1) {
            state = states[state].link;
            current_length = states[state].length;
        }

        if (states[state].next[ch] != -1) {
            state = states[state].next[ch];
            current_length++;
            if (current_length > best)
                best = current_length;
        } else {
            state = 0;
            current_length = 0;
        }
    }
    return best;
}

void history_search_unlocked(const char *term)
{
    if (term == NULL || term[0] == '\0' || history_count == 0) {
        add_text("No match for search term in history\n");
        return;
    }

    /* The most recent exact match takes priority. */
    for (int i = history_count - 1; i >= 0; i--) {
        if (strcmp(history_at(i), term) == 0) {
            add_text(history_at(i));
            add_char('\n');
            return;
        }
    }

    int scores[MAX_HISTORY];
    int best_score = 0;
    int state_count = 0;
    int term_length = (int)strlen(term);
    SuffixState *states = build_suffix_automaton(term, term_length, &state_count);
    (void)state_count; /* The builder owns this count for construction/debugging. */

    if (states == NULL) {
        add_text("terminator: could not allocate history search data\n");
        return;
    }

    for (int i = 0; i < history_count; i++) {
        scores[i] = suffix_automaton_match(states, history_at(i));
        if (scores[i] > best_score)
            best_score = scores[i];
    }
    free(states);

    if (best_score <= 2) {
        add_text("No match for search term in history\n");
        return;
    }

    /* Print all commands tied for the longest common substring, newest first. */
    for (int i = history_count - 1; i >= 0; i--) {
        if (scores[i] == best_score) {
            add_text(history_at(i));
            add_char('\n');
        }
    }
}

int history_search_collect(const char *term,
                           char results[][INPUT_CAPACITY], int capacity)
{
    if (term == NULL || term[0] == '\0' || results == NULL || capacity <= 0)
        return 0;

    pthread_mutex_lock(&history_mutex);
    if (history_count <= 0) {
        pthread_mutex_unlock(&history_mutex);
        return 0;
    }

    /* Prefer the newest exact command, matching the assignment requirement. */
    for (int i = history_count - 1; i >= 0; i--) {
        const char *entry = history_at(i);
        if (strcmp(entry, term) == 0) {
            snprintf(results[0], INPUT_CAPACITY, "%s", entry);
            pthread_mutex_unlock(&history_mutex);
            return 1;
        }
    }

    int *scores = calloc((size_t)history_count, sizeof(*scores));
    if (scores == NULL) {
        pthread_mutex_unlock(&history_mutex);
        return 0;
    }

    int state_count = 0;
    SuffixState *states = build_suffix_automaton(term, (int)strlen(term),
                                                  &state_count);
    (void)state_count;
    if (states == NULL) {
        free(scores);
        pthread_mutex_unlock(&history_mutex);
        return 0;
    }

    int best_score = 0;
    for (int i = 0; i < history_count; i++) {
        scores[i] = suffix_automaton_match(states, history_at(i));
        if (scores[i] > best_score)
            best_score = scores[i];
    }

    int found = 0;
    if (best_score > 2) {
        /* Newest-first order; retain all best-score matches up to capacity. */
        for (int i = history_count - 1; i >= 0 && found < capacity; i--) {
            if (scores[i] == best_score) {
                snprintf(results[found], INPUT_CAPACITY, "%s", history_at(i));
                found++;
            }
        }
    }

    free(states);
    free(scores);
    pthread_mutex_unlock(&history_mutex);
    return found;
}

void history_search(const char *term)
{
    pthread_mutex_lock(&history_mutex);
    history_search_unlocked(term);
    pthread_mutex_unlock(&history_mutex);
}
