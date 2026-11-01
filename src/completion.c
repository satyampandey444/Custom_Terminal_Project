#include "terminator_internal.h"

/* Completion module. */

int is_completion_delimiter(char c)
{
    return c == '\0' || c == ' ' || c == '\t' || c == '\n' ||
           c == '|' || c == '<' || c == '>';
}

int compare_completion_strings(const void *left, const void *right)
{
    const char *a = (const char *)left;
    const char *b = (const char *)right;
    return strcmp(a, b);
}

int replace_input_range(size_t start, size_t end, const char *replacement)
{
    if (start > end || end > input_len || replacement == NULL)
        return -1;

    size_t replacement_len = strlen(replacement);
    size_t suffix_len = input_len - end;
    if (start + replacement_len + suffix_len >= sizeof(input)) {
        add_text("terminator: completion would make the input too long\n");
        return -1;
    }

    memmove(input + start + replacement_len, input + end, suffix_len + 1);
    memcpy(input + start, replacement, replacement_len);
    input_len = start + replacement_len + suffix_len;
    input_cursor = start + replacement_len;
    input[input_len] = '\0';
    history_navigation = -1;
    return 0;
}

void cancel_completion_selection(void)
{
    completion_selection_mode = 0;
    completion_choice_count = 0;
    completion_choice_len = 0;
    completion_choice_input[0] = '\0';
}

void apply_completion_choice(int index)
{
    if (index < 0 || index >= completion_choice_count) {
        cancel_completion_selection();
        return;
    }

    char selected[INPUT_CAPACITY];
    snprintf(selected, sizeof(selected), "%s", completion_choices[index]);
    (void)replace_input_range(completion_replace_start,
                              completion_replace_end, selected);
    cancel_completion_selection();
}

void complete_filename(void)
{
    if (search_mode || continuation_mode || completion_selection_mode || input_cursor == 0)
        return;

    size_t word_start = input_cursor;
    while (word_start > 0 && !is_completion_delimiter(input[word_start - 1]))
        word_start--;

    size_t word_end = input_cursor;
    while (word_end < input_len && !is_completion_delimiter(input[word_end]))
        word_end++;

    if (word_start == input_cursor)
        return;

    /* Quoted/escaped filenames need shell-aware word parsing; leave those untouched for now. */
    for (size_t i = word_start; i < word_end; i++) {
        if (input[i] == '\'' || input[i] == '"' || input[i] == '\\')
            return;
    }

    char typed_prefix[INPUT_CAPACITY];
    size_t typed_len = input_cursor - word_start;
    if (typed_len == 0 || typed_len >= sizeof(typed_prefix))
        return;
    memcpy(typed_prefix, input + word_start, typed_len);
    typed_prefix[typed_len] = '\0';

    char directory_part[INPUT_CAPACITY];
    char search_directory[INPUT_CAPACITY];
    char basename_prefix[INPUT_CAPACITY];
    const char *slash = strrchr(typed_prefix, '/');
    if (slash != NULL) {
        size_t dirpart_len = (size_t)(slash - typed_prefix) + 1;
        if (dirpart_len >= sizeof(directory_part))
            return;
        memcpy(directory_part, typed_prefix, dirpart_len);
        directory_part[dirpart_len] = '\0';

        size_t directory_len = dirpart_len - 1;
        if (typed_prefix[0] == '/') {
            if (directory_len == 0) {
                snprintf(search_directory, sizeof(search_directory), "/");
            } else {
                memcpy(search_directory, typed_prefix, directory_len);
                search_directory[directory_len] = '\0';
            }
        } else {
            char cwd[PATH_MAX];
            copy_tab_cwd(current_tab_slot(), cwd, sizeof(cwd));
            char relative_dir[INPUT_CAPACITY];
            if (directory_len >= sizeof(relative_dir))
                return;
            memcpy(relative_dir, typed_prefix, directory_len);
            relative_dir[directory_len] = '\0';
            int written = snprintf(search_directory, sizeof(search_directory),
                                   "%s/%s", cwd, relative_dir);
            if (written < 0 || (size_t)written >= sizeof(search_directory))
                return;
        }
        snprintf(basename_prefix, sizeof(basename_prefix), "%s", slash + 1);
    } else {
        directory_part[0] = '\0';
        copy_tab_cwd(current_tab_slot(), search_directory, sizeof(search_directory));
        snprintf(basename_prefix, sizeof(basename_prefix), "%s", typed_prefix);
    }

    /* Avoid dumping every file when Tab is pressed on an empty basename. */
    if (basename_prefix[0] == '\0')
        return;

    DIR *dir = opendir(search_directory);
    if (dir == NULL)
        return;

    completion_choice_count = 0;
    struct dirent *entry;
    size_t basename_prefix_len = strlen(basename_prefix);

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (name[0] == '.' && basename_prefix[0] != '.')
            continue;
        if (strncmp(name, basename_prefix, basename_prefix_len) != 0)
            continue;
        if (completion_choice_count >= MAX_COMPLETIONS)
            continue;

        char candidate[INPUT_CAPACITY];
        int written = snprintf(candidate, sizeof(candidate), "%s%s",
                               directory_part, name);
        if (written < 0 || (size_t)written >= sizeof(candidate))
            continue;

        char filesystem_path[INPUT_CAPACITY];
        /* candidate is the already bounds-checked directory + entry name. */
        snprintf(filesystem_path, sizeof(filesystem_path), "%s", candidate);

        struct stat info;
        if (stat(filesystem_path, &info) == 0 && S_ISDIR(info.st_mode)) {
            size_t length = strlen(candidate);
            if (length + 1 < sizeof(candidate)) {
                candidate[length] = '/';
                candidate[length + 1] = '\0';
            }
        }

        snprintf(completion_choices[completion_choice_count],
                 sizeof(completion_choices[completion_choice_count]),
                 "%s", candidate);
        completion_choice_count++;
    }
    closedir(dir);

    if (completion_choice_count == 0)
        return;

    qsort(completion_choices, (size_t)completion_choice_count,
          sizeof(completion_choices[0]), compare_completion_strings);

    if (completion_choice_count == 1) {
        char only_match[INPUT_CAPACITY];
        snprintf(only_match, sizeof(only_match), "%s", completion_choices[0]);
        (void)replace_input_range(word_start, word_end, only_match);
        completion_choice_count = 0;
        return;
    }

    /* Find the longest common prefix shared by all candidate paths. */
    size_t common_len = strlen(completion_choices[0]);
    for (int i = 1; i < completion_choice_count; i++) {
        size_t j = 0;
        while (j < common_len && completion_choices[i][j] != '\0' &&
               completion_choices[i][j] == completion_choices[0][j])
            j++;
        common_len = j;
    }

    char common_prefix[INPUT_CAPACITY];
    if (common_len >= sizeof(common_prefix))
        common_len = sizeof(common_prefix) - 1;
    memcpy(common_prefix, completion_choices[0], common_len);
    common_prefix[common_len] = '\0';

    if (replace_input_range(word_start, word_end, common_prefix) != 0) {
        completion_choice_count = 0;
        return;
    }

    /* Ambiguity remains: show all bounded candidates and allow numeric selection. */
    completion_replace_start = word_start;
    completion_replace_end = word_start + common_len;
    completion_selection_mode = 1;
    completion_choice_len = 0;
    completion_choice_input[0] = '\0';

    add_text("Possible completions:\n");
    for (int i = 0; i < completion_choice_count; i++) {
        char row[INPUT_CAPACITY + 32];
        snprintf(row, sizeof(row), "%3d. %.4090s\n", i + 1, completion_choices[i]);
        add_text(row);
    }
    add_text("Type a number and press Enter to select, or Esc to cancel.\n");
}

void handle_completion_selection(KeySym key, const char *chars, int n)
{
    if (key == XK_Escape) {
        cancel_completion_selection();
        draw_window();
        return;
    }

    if (key == XK_BackSpace || key == XK_Delete) {
        if (completion_choice_len > 0)
            completion_choice_input[--completion_choice_len] = '\0';
        draw_window();
        return;
    }

    if (key == XK_Return || key == XK_KP_Enter) {
        if (completion_choice_len == 0) {
            cancel_completion_selection();
        } else {
            char *endptr = NULL;
            long choice = strtol(completion_choice_input, &endptr, 10);
            if (endptr != completion_choice_input && *endptr == '\0' &&
                choice >= 1 && choice <= completion_choice_count) {
                apply_completion_choice((int)choice - 1);
            } else {
                add_text("terminator: invalid completion selection\n");
                cancel_completion_selection();
            }
        }
        draw_window();
        return;
    }

    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (chars[i] >= '0' && chars[i] <= '9' &&
                completion_choice_len + 1 < sizeof(completion_choice_input)) {
                completion_choice_input[completion_choice_len++] = chars[i];
                completion_choice_input[completion_choice_len] = '\0';
            }
        }
    }
    draw_window();
}
