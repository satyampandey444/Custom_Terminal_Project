#include "terminator_internal.h"

/* Parser module. */

int tokenize(const char *line, Token tokens[MAX_TOKENS], int *count)
{
    size_t i = 0;
    int n = 0;

    while (line[i] != '\0') {
        while (line[i] == ' ' || line[i] == '\t' || line[i] == '\n')
            i++;

        if (line[i] == '\0')
            break;

        if (n >= MAX_TOKENS) {
            add_text("terminator: too many tokens\n");
            return -1;
        }

        /* Operators only have special meaning when they are unquoted. */
        if (line[i] == '|' || line[i] == '<' || line[i] == '>') {
            tokens[n].text[0] = line[i++];
            tokens[n].text[1] = '\0';
            tokens[n].is_operator = 1;
            n++;
            continue;
        }

        size_t j = 0;
        int quote = 0;
        int token_started = 0;

        while (line[i] != '\0') {
            char c = line[i];

            if (quote == 0) {
                if (c == ' ' || c == '\t' || c == '\n' ||
                    c == '|' || c == '<' || c == '>') {
                    break;
                }

                if (c == '\'' || c == '"') {
                    quote = (unsigned char)c;
                    token_started = 1;
                    i++;
                    continue;
                }

                if (c == '\\') {
                    if (line[i + 1] == '\0') {
                        add_text("terminator: trailing backslash\n");
                        return -1;
                    }
                    c = line[i + 1];
                    i += 2;
                } else {
                    i++;
                }
            } else if (quote == '\'') {
                if (c == '\'') {
                    quote = 0;
                    i++;
                    continue;
                }
                i++;
            } else { /* Inside double quotes. */
                if (c == '"') {
                    quote = 0;
                    i++;
                    continue;
                }
                if (c == '\\' && line[i + 1] != '\0') {
                    c = line[i + 1];
                    i += 2;
                } else {
                    i++;
                }
            }

            token_started = 1;
            if (j >= MAX_TOKEN_LEN - 1) {
                add_text("terminator: token too long\n");
                return -1;
            }
            tokens[n].text[j++] = c;
        }

        if (quote != 0) {
            add_text("terminator: unmatched quote\n");
            return -1;
        }

        if (token_started) {
            tokens[n].text[j] = '\0';
            tokens[n].is_operator = 0;
            n++;
        }
    }

    *count = n;
    return 0;
}

int parse_command(
    Token tokens[MAX_TOKENS],
    int token_count,
    ParsedCommand commands[MAX_COMMANDS],
    int *command_count)
{
    memset(commands, 0,
           sizeof(ParsedCommand) * MAX_COMMANDS);

    int current = 0;

    for (int i = 0; i < token_count; i++) {
        ParsedCommand *cmd = &commands[current];

        if (tokens[i].is_operator && strcmp(tokens[i].text, "|") == 0) {
            if (cmd->argc == 0) {
                add_text("terminator: missing command before pipe\n");
                return -1;
            }

            if (current >= MAX_COMMANDS - 1) {
                add_text("terminator: too many pipeline commands\n");
                return -1;
            }

            current++;
            continue;
        }

        if (tokens[i].is_operator &&
            (strcmp(tokens[i].text, "<") == 0 ||
             strcmp(tokens[i].text, ">") == 0)) {
            int is_input = strcmp(tokens[i].text, "<") == 0;

            if (i + 1 >= token_count ||
                (tokens[i + 1].is_operator &&
                 (strcmp(tokens[i + 1].text, "|") == 0 ||
                  strcmp(tokens[i + 1].text, "<") == 0 ||
                  strcmp(tokens[i + 1].text, ">") == 0))) {
                add_text("terminator: missing redirection filename\n");
                return -1;
            }

            if (is_input) {
                if (cmd->infile != NULL) {
                    add_text("terminator: duplicate input redirection\n");
                    return -1;
                }
                cmd->infile = tokens[++i].text;
            } else {
                if (cmd->outfile != NULL) {
                    add_text("terminator: duplicate output redirection\n");
                    return -1;
                }
                cmd->outfile = tokens[++i].text;
            }

            continue;
        }

        if (cmd->argc >= MAX_ARGS - 1) {
            add_text("terminator: too many arguments\n");
            return -1;
        }

        cmd->argv[cmd->argc++] = tokens[i].text;
        cmd->argv[cmd->argc] = NULL;
    }

    if (commands[current].argc == 0) {
        add_text("terminator: missing command\n");
        return -1;
    }

    *command_count = current + 1;
    return 0;
}

int append_char_bounded(char *dest, size_t capacity, size_t *used, char c)
{
    if (*used + 1 >= capacity)
        return -1;
    dest[(*used)++] = c;
    dest[*used] = '\0';
    return 0;
}

int append_expansion_value(char *dest, size_t capacity, size_t *used,
                                  const char *value, int quote_context)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        char c = (char)*p;
        int escape = 0;
        if (quote_context == '"') {
            escape = (c == '"' || c == '\\');
        } else {
            escape = (c == '\\' || c == '\'' || c == '"' ||
                      c == '|' || c == '<' || c == '>');
        }
        if (escape && append_char_bounded(dest, capacity, used, '\\') != 0)
            return -1;
        if (append_char_bounded(dest, capacity, used, c) != 0)
            return -1;
    }
    return 0;
}

int execute_captured_command(const char *command, char *result,
                                    size_t result_capacity, int depth)
{
    if (result_capacity == 0)
        return 1;
    result[0] = '\0';
    if (depth > MAX_EXPANSION_DEPTH) {
        add_text("terminator: command substitution nesting too deep\n");
        return 1;
    }

    char expanded[MAX_COMMAND_SIZE];
    if (expand_shell_text(command, expanded, sizeof(expanded), depth + 1) != 0) {
        add_text("terminator: command substitution is too large\n");
        return 1;
    }

    Token tokens[MAX_TOKENS];
    int token_count = 0;
    if (tokenize(expanded, tokens, &token_count) != 0 || token_count == 0)
        return 1;

    ParsedCommand commands[MAX_COMMANDS];
    int command_count = 0;
    if (parse_command(tokens, token_count, commands, &command_count) != 0)
        return 1;

    /* Capture child output without allowing a nested command to change the
     * status seen by the surrounding shell until that outer command finishes. */
    int previous_status = last_exit_status;
    int status = execute_pipeline_internal(commands, command_count,
                                           result, result_capacity);
    (void)status;
    last_exit_status = previous_status;

    /* Bash removes trailing newlines from command-substitution output. */
    size_t length = strlen(result);
    while (length > 0 && result[length - 1] == '\n')
        result[--length] = '\0';
    return 0;
}

int expand_shell_text(const char *source, char *dest, size_t capacity,
                             int depth)
{
    if (capacity == 0 || depth > MAX_EXPANSION_DEPTH)
        return -1;
    size_t used = 0;
    int quote = 0;
    char cwd_expansion[PATH_MAX];
    dest[0] = '\0';

    for (size_t i = 0; source[i] != '\0';) {
        char c = source[i];

        if (quote == '\'') {
            if (append_char_bounded(dest, capacity, &used, c) != 0)
                return -1;
            if (c == '\'')
                quote = 0;
            i++;
            continue;
        }

        if (c == '\\') {
            if (append_char_bounded(dest, capacity, &used, c) != 0)
                return -1;
            if (source[i + 1] != '\0') {
                if (append_char_bounded(dest, capacity, &used, source[i + 1]) != 0)
                    return -1;
                i += 2;
            } else {
                i++;
            }
            continue;
        }

        if (c == '\'') {
            quote = '\'';
            if (append_char_bounded(dest, capacity, &used, c) != 0)
                return -1;
            i++;
            continue;
        }
        if (c == '"') {
            quote = (quote == '"') ? 0 : '"';
            if (append_char_bounded(dest, capacity, &used, c) != 0)
                return -1;
            i++;
            continue;
        }

        if (quote != '\'' && c == '`') {
            const char *end = find_backtick_end(source + i + 1);
            if (end == NULL) {
                if (append_char_bounded(dest, capacity, &used, c) != 0)
                    return -1;
                i++;
                continue;
            }
            size_t inner_len = (size_t)(end - (source + i + 1));
            if (inner_len >= MAX_COMMAND_SIZE)
                return -1;
            char inner[MAX_COMMAND_SIZE];
            memcpy(inner, source + i + 1, inner_len);
            inner[inner_len] = '\0';
            char result[MAX_SUBSTITUTION_OUTPUT];
            if (execute_captured_command(inner, result, sizeof(result), depth + 1) != 0)
                result[0] = '\0';
            if (append_expansion_value(dest, capacity, &used, result, quote) != 0)
                return -1;
            i = (size_t)(end - source) + 1;
            continue;
        }

        if (c == '$' && source[i + 1] == '(') {
            const char *end = find_substitution_end(source + i + 2);
            if (end == NULL) {
                if (append_char_bounded(dest, capacity, &used, c) != 0)
                    return -1;
                i++;
                continue;
            }
            size_t inner_len = (size_t)(end - (source + i + 2));
            if (inner_len >= MAX_COMMAND_SIZE)
                return -1;
            char inner[MAX_COMMAND_SIZE];
            memcpy(inner, source + i + 2, inner_len);
            inner[inner_len] = '\0';
            char result[MAX_SUBSTITUTION_OUTPUT];
            if (execute_captured_command(inner, result, sizeof(result), depth + 1) != 0)
                result[0] = '\0';
            if (append_expansion_value(dest, capacity, &used, result, quote) != 0)
                return -1;
            i = (size_t)(end - source) + 1;
            continue;
        }

        if (c == '$') {
            const char *value = NULL;
            char variable_name[256];
            size_t consumed = 1;
            variable_name[0] = '\0';

            if (source[i + 1] == '?') {
                snprintf(variable_name, sizeof(variable_name), "%d", last_exit_status);
                value = variable_name;
                consumed = 2;
            } else if (source[i + 1] == '$') {
                snprintf(variable_name, sizeof(variable_name), "%ld", (long)getpid());
                value = variable_name;
                consumed = 2;
            } else if (source[i + 1] == '{') {
                size_t j = i + 2;
                while (source[j] != '\0' && source[j] != '}' &&
                       j - (i + 2) < sizeof(variable_name) - 1)
                    j++;
                if (source[j] == '}') {
                    size_t len = j - (i + 2);
                    memcpy(variable_name, source + i + 2, len);
                    variable_name[len] = '\0';
                    if (strcmp(variable_name, "PWD") == 0) {
                        copy_tab_cwd(current_tab_slot(), cwd_expansion, sizeof(cwd_expansion));
                        value = cwd_expansion;
                    } else {
                        value = getenv(variable_name);
                    }
                    if (value == NULL)
                        value = "";
                    consumed = j - i + 1;
                }
            } else if (isalpha((unsigned char)source[i + 1]) || source[i + 1] == '_') {
                size_t j = i + 1;
                size_t len = 0;
                while ((isalnum((unsigned char)source[j]) || source[j] == '_') &&
                       len < sizeof(variable_name) - 1) {
                    variable_name[len++] = source[j++];
                }
                variable_name[len] = '\0';
                if (strcmp(variable_name, "PWD") == 0) {
                    copy_tab_cwd(current_tab_slot(), cwd_expansion, sizeof(cwd_expansion));
                    value = cwd_expansion;
                } else {
                    value = getenv(variable_name);
                }
                if (value == NULL)
                    value = "";
                consumed = j - i;
            }

            if (value != NULL) {
                if (append_expansion_value(dest, capacity, &used, value, quote) != 0)
                    return -1;
                i += consumed;
                continue;
            }
        }

        if (append_char_bounded(dest, capacity, &used, c) != 0)
            return -1;
        i++;
    }
    return 0;
}

int command_needs_continuation(const char *command, int *backslash_join)
{
    int quote = 0;
    int backtick = 0;
    int substitution_depth = 0;
    int trailing_backslash = 0;

    for (size_t i = 0; command[i] != '\0'; i++) {
        char c = command[i];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            continue;
        }
        if (c == '\\') {
            if (command[i + 1] == '\0') {
                trailing_backslash = (quote != '\'');
                break;
            }
            i++;
            continue;
        }
        if (backtick) {
            if (c == '`') backtick = 0;
            continue;
        }
        if (c == '`' && quote != '\'') {
            backtick = 1;
            continue;
        }
        if (c == '\'') {
            quote = '\'';
            continue;
        }
        if (c == '"') {
            quote = (quote == '"') ? 0 : '"';
            continue;
        }
        if (c == '$' && command[i + 1] == '(') {
            substitution_depth++;
            i++;
            continue;
        }
        if (substitution_depth > 0 && c == '(') {
            substitution_depth++;
            continue;
        }
        if (substitution_depth > 0 && c == ')') {
            substitution_depth--;
            continue;
        }
    }

    if (backslash_join != NULL)
        *backslash_join = trailing_backslash;
    return trailing_backslash || quote != 0 || backtick || substitution_depth > 0;
}

void clear_current_input(void)
{
    input_len = 0;
    input_cursor = 0;
    input[0] = '\0';
    history_navigation = -1;
}

void submit_command_line(void)
{
    input[input_len] = '\0';
    const char *shown_prompt = continuation_mode ? "> " : "user@terminator> ";
    add_text(shown_prompt);
    add_text(input);
    add_char('\n');

    size_t needed = pending_command_len + input_len;
    if (needed + 2 >= sizeof(pending_command)) {
        add_text("terminator: multiline command is too long\n");
        pending_command[0] = '\0';
        pending_command_len = 0;
        continuation_mode = 0;
        clear_current_input();
        return;
    }

    memcpy(pending_command + pending_command_len, input, input_len);
    pending_command_len += input_len;
    pending_command[pending_command_len] = '\0';

    int backslash_join = 0;
    if (command_needs_continuation(pending_command, &backslash_join)) {
        if (backslash_join && pending_command_len > 0) {
            pending_command[--pending_command_len] = '\0';
        } else {
            pending_command[pending_command_len++] = '\n';
            pending_command[pending_command_len] = '\0';
        }
        continuation_mode = 1;
        clear_current_input();
        return;
    }

    char command[MAX_COMMAND_SIZE];
    memcpy(command, pending_command, pending_command_len + 1);
    pending_command[0] = '\0';
    pending_command_len = 0;
    continuation_mode = 0;
    clear_current_input();

    if (!is_blank_command(command))
        history_record(command);
    dispatch_command(command);
}
