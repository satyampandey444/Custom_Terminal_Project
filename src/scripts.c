#include "terminator_internal.h"

/* Scripts module. */

char *script_trim(char *text)
{
    while (isspace((unsigned char)*text))
        text++;
    size_t n = strlen(text);
    while (n > 0 && isspace((unsigned char)text[n - 1]))
        text[--n] = '\0';
    return text;
}

void script_remove_comment(char *line)
{
    int quote = 0;
    for (size_t i = 0; line[i] != '\0'; i++) {
        if (quote == '\'' ) {
            if (line[i] == '\'')
                quote = 0;
            continue;
        }
        if (quote == '"') {
            if (line[i] == '"')
                quote = 0;
            else if (line[i] == '\\' && line[i + 1] != '\0')
                i++;
            continue;
        }
        if (line[i] == '\'') {
            quote = '\'';
            continue;
        }
        if (line[i] == '"') {
            quote = '"';
            continue;
        }
        if (line[i] == '#' && (i == 0 || isspace((unsigned char)line[i - 1]))) {
            line[i] = '\0';
            return;
        }
    }
}

int script_keyword(const char *line, const char *keyword)
{
    size_t n = strlen(keyword);
    return strncmp(line, keyword, n) == 0 &&
           (line[n] == '\0' || isspace((unsigned char)line[n]) || line[n] == '[' || line[n] == ';');
}

int script_has_then(const char *line)
{
    int quote = 0;
    for (size_t i = 0; line[i] != '\0'; i++) {
        if (quote == '\'') {
            if (line[i] == '\'') quote = 0;
            continue;
        }
        if (quote == '"') {
            if (line[i] == '"') quote = 0;
            else if (line[i] == '\\' && line[i + 1] != '\0') i++;
            continue;
        }
        if (line[i] == '\'') { quote = '\''; continue; }
        if (line[i] == '"') { quote = '"'; continue; }
        if ((i == 0 || isspace((unsigned char)line[i - 1]) || line[i - 1] == ';') &&
            strncmp(line + i, "then", 4) == 0 &&
            (line[i + 4] == '\0' || isspace((unsigned char)line[i + 4]) || line[i + 4] == ';'))
            return 1;
    }
    return 0;
}

int script_line_is_assignment(const char *line, char *name, size_t name_cap,
                                     char *value, size_t value_cap)
{
    const char *eq = strchr(line, '=');
    if (eq == NULL || eq == line)
        return 0;
    /* Assignment syntax must be a single shell word with no whitespace before '='. */
    for (const char *p = line; p < eq; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    }
    if (!(isalpha((unsigned char)line[0]) || line[0] == '_')) return 0;
    size_t n = (size_t)(eq - line);
    if (n >= name_cap) return 0;
    memcpy(name, line, n);
    name[n] = '\0';
    if (snprintf(value, value_cap, "%s", eq + 1) >= (int)value_cap)
        return -1;
    return 1;
}

const char *script_get_variable(const ScriptContext *ctx, const char *name)
{
    for (int i = ctx->variable_count - 1; i >= 0; i--) {
        if (strcmp(ctx->variables[i].name, name) == 0)
            return ctx->variables[i].value;
    }
    const char *environment = getenv(name);
    return environment != NULL ? environment : "";
}

int script_set_variable(ScriptContext *ctx, const char *name, const char *value)
{
    for (int i = ctx->variable_count - 1; i >= 0; i--) {
        if (strcmp(ctx->variables[i].name, name) == 0) {
            if (snprintf(ctx->variables[i].value, sizeof(ctx->variables[i].value),
                         "%s", value) >= (int)sizeof(ctx->variables[i].value))
                return -1;
            return 0;
        }
    }
    if (ctx->variable_count >= SCRIPT_MAX_VARIABLES)
        return -1;
    ScriptVariable *v = &ctx->variables[ctx->variable_count++];
    if (snprintf(v->name, sizeof(v->name), "%s", name) >= (int)sizeof(v->name) ||
        snprintf(v->value, sizeof(v->value), "%s", value) >= (int)sizeof(v->value))
        return -1;
    return 0;
}

int script_append(char *dest, size_t cap, size_t *used, const char *text, size_t n)
{
    if (*used + n >= cap)
        return -1;
    memcpy(dest + *used, text, n);
    *used += n;
    dest[*used] = '\0';
    return 0;
}

int script_expand_variables(const char *line, ScriptContext *ctx,
                                   char *dest, size_t capacity)
{
    size_t used = 0;
    int quote = 0;
    dest[0] = '\0';
    for (size_t i = 0; line[i] != '\0';) {
        char c = line[i];
        if (quote == '\'') {
            if (script_append(dest, capacity, &used, &line[i], 1) != 0) return -1;
            if (c == '\'') quote = 0;
            i++;
            continue;
        }
        if (c == '\'' && quote == 0) {
            quote = '\'';
            if (script_append(dest, capacity, &used, &line[i++], 1) != 0) return -1;
            continue;
        }
        if (c == '"' && quote == 0) quote = '"';
        else if (c == '"' && quote == '"') quote = 0;

        if (c == '\\' && line[i + 1] != '\0') {
            if (script_append(dest, capacity, &used, line + i, 2) != 0) return -1;
            i += 2;
            continue;
        }
        if (c != '$') {
            if (script_append(dest, capacity, &used, line + i, 1) != 0) return -1;
            i++;
            continue;
        }

        size_t start = i;
        i++;
        char name[128];
        name[0] = '\0';
        const char *value = "";
        char numeric[64];
        if (line[i] == '?') {
            snprintf(numeric, sizeof(numeric), "%d", ctx->status);
            value = numeric;
            i++;
        } else if (line[i] == '#') {
            snprintf(numeric, sizeof(numeric), "%d", ctx->arg_count);
            value = numeric;
            i++;
        } else if (line[i] == '0') {
            value = ctx->script_path;
            i++;
        } else if (line[i] >= '1' && line[i] <= '9') {
            int arg_index = line[i] - '1';
            value = arg_index < ctx->arg_count ? ctx->args[arg_index] : "";
            i++;
        } else {
            size_t n = 0;
            if (line[i] == '{') {
                i++;
                while (line[i] != '\0' && line[i] != '}' && n + 1 < sizeof(name))
                    name[n++] = line[i++];
                if (line[i] == '}') i++;
                else {
                    if (script_append(dest, capacity, &used, line + start, strlen(line + start)) != 0) return -1;
                    break;
                }
            } else if (isalpha((unsigned char)line[i]) || line[i] == '_') {
                while ((isalnum((unsigned char)line[i]) || line[i] == '_') && n + 1 < sizeof(name))
                    name[n++] = line[i++];
            } else {
                if (script_append(dest, capacity, &used, "$", 1) != 0) return -1;
                continue;
            }
            name[n] = '\0';
            value = script_get_variable(ctx, name);
        }
        if (script_append(dest, capacity, &used, value, strlen(value)) != 0)
            return -1;
    }
    return 0;
}

int script_eval_test(char *condition, ScriptContext *ctx)
{
    char expanded[MAX_COMMAND_SIZE];
    if (script_expand_variables(condition, ctx, expanded, sizeof(expanded)) != 0)
        return 0;
    Token tokens[MAX_TOKENS];
    int token_count = 0;
    if (tokenize(expanded, tokens, &token_count) != 0 || token_count < 3)
        return 0;
    int first = tokens[0].is_operator == 0 && strcmp(tokens[0].text, "[") == 0;
    int last = token_count > 0 && strcmp(tokens[token_count - 1].text, "]") == 0;
    if (!first || !last) return 0;
    int n = token_count - 2;
    char **v = calloc((size_t)n, sizeof(*v));
    if (v == NULL) return 0;
    for (int i = 0; i < n; i++) v[i] = tokens[i + 1].text;
    int result = 0;
    if (n == 2 && strcmp(v[0], "-f") == 0) {
        struct stat st; result = stat(v[1], &st) == 0 && S_ISREG(st.st_mode);
    } else if (n == 2 && strcmp(v[0], "-d") == 0) {
        struct stat st; result = stat(v[1], &st) == 0 && S_ISDIR(st.st_mode);
    } else if (n == 3 && (strcmp(v[1], "=") == 0 || strcmp(v[1], "==") == 0)) {
        result = strcmp(v[0], v[2]) == 0;
    } else if (n == 3 && strcmp(v[1], "!=") == 0) {
        result = strcmp(v[0], v[2]) != 0;
    } else if (n == 3 && (strcmp(v[1], "-eq") == 0 || strcmp(v[1], "-gt") == 0 ||
                          strcmp(v[1], "-ne") == 0 || strcmp(v[1], "-lt") == 0 ||
                          strcmp(v[1], "-ge") == 0 || strcmp(v[1], "-le") == 0)) {
        char *end1 = NULL, *end2 = NULL;
        long a = strtol(v[0], &end1, 10), b = strtol(v[2], &end2, 10);
        if (end1 != v[0] && *end1 == '\0' && end2 != v[2] && *end2 == '\0') {
            if (strcmp(v[1], "-eq") == 0) result = a == b;
            else if (strcmp(v[1], "-gt") == 0) result = a > b;
            else if (strcmp(v[1], "-ne") == 0) result = a != b;
            else if (strcmp(v[1], "-lt") == 0) result = a < b;
            else if (strcmp(v[1], "-ge") == 0) result = a >= b;
            else result = a <= b;
        }
    } else if (n == 1) {
        result = v[0][0] != '\0';
    }
    free(v);
    return result;
}

int script_is_if(const char *line) { return script_keyword(line, "if"); }

int script_is_elif(const char *line) { return script_keyword(line, "elif"); }

int script_is_else(const char *line) { return script_keyword(line, "else"); }

int script_is_fi(const char *line) { return script_keyword(line, "fi"); }

int script_is_then(const char *line) { return script_keyword(line, "then"); }

int script_validate_structure(char **lines, size_t count, size_t *bad_line)
{
    int depth = 0;
    int has_else[SCRIPT_MAX_IF_DEPTH];
    int needs_then[SCRIPT_MAX_IF_DEPTH];
    for (size_t i = 0; i < count; i++) {
        char copy[SCRIPT_LINE_CAPACITY];
        snprintf(copy, sizeof(copy), "%s", lines[i]);
        script_remove_comment(copy);
        char *line = script_trim(copy);
        if (*line == '\0' || line[0] == '#') continue;
        if (script_is_if(line)) {
            if (depth >= SCRIPT_MAX_IF_DEPTH) { *bad_line = i + 1; return -1; }
            has_else[depth] = 0;
            needs_then[depth] = !script_has_then(line);
            depth++;
        } else if (script_is_elif(line)) {
            if (depth == 0 || has_else[depth - 1]) { *bad_line = i + 1; return -1; }
            needs_then[depth - 1] = !script_has_then(line);
        } else if (script_is_else(line)) {
            if (depth == 0 || has_else[depth - 1] || needs_then[depth - 1]) { *bad_line = i + 1; return -1; }
            has_else[depth - 1] = 1;
        } else if (script_is_then(line)) {
            if (depth == 0 || !needs_then[depth - 1]) { *bad_line = i + 1; return -1; }
            needs_then[depth - 1] = 0;
        } else if (script_is_fi(line)) {
            if (depth == 0 || needs_then[depth - 1]) { *bad_line = i + 1; return -1; }
            depth--;
        }
    }
    if (depth != 0) { *bad_line = count; return -1; }
    return 0;
}

int script_find_then_line(char **lines, size_t count, size_t from, size_t *body_start)
{
    for (size_t i = from; i < count; i++) {
        char copy[SCRIPT_LINE_CAPACITY];
        snprintf(copy, sizeof(copy), "%s", lines[i]);
        script_remove_comment(copy);
        char *line = script_trim(copy);
        if (*line == '\0') continue;
        if (script_is_then(line)) { *body_start = i + 1; return 0; }
        return -1;
    }
    return -1;
}

int script_extract_condition(const char *line, const char *keyword,
                                    char *condition, size_t capacity,
                                    int *inline_then)
{
    const char *p = line + strlen(keyword);
    while (isspace((unsigned char)*p)) p++;
    const char *end = NULL;
    int quote = 0;
    for (const char *q = p; *q; q++) {
        if (quote == '\'') { if (*q == '\'') quote = 0; continue; }
        if (quote == '"') { if (*q == '"') quote = 0; else if (*q == '\\' && q[1]) q++; continue; }
        if (*q == '\'') { quote = '\''; continue; }
        if (*q == '"') { quote = '"'; continue; }
        if ((q == p || isspace((unsigned char)q[-1]) || q[-1] == ';') &&
            strncmp(q, "then", 4) == 0 &&
            (q[4] == '\0' || isspace((unsigned char)q[4]) || q[4] == ';')) {
            end = q;
            *inline_then = 1;
            break;
        }
    }
    if (end == NULL) {
        end = p + strlen(p);
        *inline_then = 0;
    }
    while (end > p && (isspace((unsigned char)end[-1]) || end[-1] == ';')) end--;
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= capacity) return -1;
    memcpy(condition, p, n);
    condition[n] = '\0';
    return 0;
}

int script_execute_if(int slot, char **lines, size_t count, size_t *index,
                             ScriptContext *ctx, int *exit_status)
{
    ScriptBranch branches[SCRIPT_MAX_IF_DEPTH];
    int branch_count = 1;
    memset(branches, 0, sizeof(branches));
    size_t if_line = *index;
    char first[SCRIPT_LINE_CAPACITY];
    snprintf(first, sizeof(first), "%s", lines[if_line]);
    script_remove_comment(first);
    char *header = script_trim(first);
    int inline_then = 0;
    if (script_extract_condition(header, "if", branches[0].condition,
                                 sizeof(branches[0].condition), &inline_then) != 0) {
        add_text("terminator: invalid if condition\n"); *exit_status = 2; return -1;
    }
    branches[0].header = if_line;
    if (inline_then) branches[0].body_start = if_line + 1;
    else if (script_find_then_line(lines, count, if_line + 1, &branches[0].body_start) != 0) {
        add_text("terminator: expected then after if\n"); *exit_status = 2; return -1;
    }

    size_t fi_line = count;
    int nested = 1;
    for (size_t j = branches[0].body_start; j < count; j++) {
        char copy[SCRIPT_LINE_CAPACITY];
        snprintf(copy, sizeof(copy), "%s", lines[j]);
        script_remove_comment(copy);
        char *line = script_trim(copy);
        if (script_is_if(line)) { nested++; continue; }
        if (script_is_fi(line)) {
            nested--;
            if (nested == 0) { fi_line = j; break; }
            continue;
        }
        if (nested == 1 && (script_is_elif(line) || script_is_else(line))) {
            if (branch_count >= SCRIPT_MAX_IF_DEPTH) {
                add_text("terminator: too many if branches\n"); *exit_status = 2; return -1;
            }
            branches[branch_count - 1].body_end = j;
            ScriptBranch *b = &branches[branch_count];
            b->header = j;
            b->is_else = script_is_else(line);
            if (!b->is_else) {
                int has_then = 0;
                if (script_extract_condition(line, "elif", b->condition,
                                             sizeof(b->condition), &has_then) != 0) {
                    add_text("terminator: invalid elif condition\n"); *exit_status = 2; return -1;
                }
                if (has_then) b->body_start = j + 1;
                else if (script_find_then_line(lines, count, j + 1, &b->body_start) != 0) {
                    add_text("terminator: expected then after elif\n"); *exit_status = 2; return -1;
                }
            } else {
                b->body_start = j + 1;
            }
            branch_count++;
            j = b->body_start - 1;
        }
    }
    if (fi_line == count) {
        add_text("terminator: missing fi\n"); *exit_status = 2; return -1;
    }
    branches[branch_count - 1].body_end = fi_line;

    int selected = -1;
    for (int b = 0; b < branch_count; b++) {
        if (branches[b].is_else || script_eval_test(branches[b].condition, ctx)) {
            selected = b;
            break;
        }
    }
    int status = 0;
    if (selected >= 0) {
        if (script_execute_range(slot, lines, branches[selected].body_start,
                                 branches[selected].body_end, ctx, &status) != 0) {
            *exit_status = status;
            return -1;
        }
    }
    *index = fi_line + 1;
    *exit_status = ctx->exited ? ctx->exit_status : ctx->status;
    return 0;
}

int script_execute_range(int slot, char **lines, size_t start, size_t end,
                                ScriptContext *ctx, int *exit_status)
{
    size_t i = start;
    while (i < end && !ctx->exited) {
        char original[SCRIPT_LINE_CAPACITY];
        snprintf(original, sizeof(original), "%s", lines[i]);
        script_remove_comment(original);
        char *line = script_trim(original);
        i++;
        if (*line == '\0' || line[0] == '#') continue;
        if (script_is_if(line)) {
            i--;
            if (script_execute_if(slot, lines, end, &i, ctx, exit_status) != 0)
                return -1;
            continue;
        }
        if (script_is_then(line) || script_is_else(line) || script_is_elif(line) || script_is_fi(line))
            continue;

        char name[128], raw_value[INPUT_CAPACITY], expanded_value[INPUT_CAPACITY];
        int assignment = script_line_is_assignment(line, name, sizeof(name),
                                                   raw_value, sizeof(raw_value));
        if (assignment < 0) {
            add_text("terminator: script assignment value too long\n");
            ctx->status = 2;
            continue;
        }
        if (assignment > 0) {
            if (script_expand_variables(raw_value, ctx, expanded_value, sizeof(expanded_value)) != 0) {
                add_text("terminator: script variable expansion too long\n");
                ctx->status = 2;
                continue;
            }
            size_t n = strlen(expanded_value);
            if (n >= 2 && ((expanded_value[0] == '\'' && expanded_value[n - 1] == '\'') ||
                           (expanded_value[0] == '"' && expanded_value[n - 1] == '"'))) {
                memmove(expanded_value, expanded_value + 1, n - 2);
                expanded_value[n - 2] = '\0';
            }
            if (script_set_variable(ctx, name, expanded_value) != 0) {
                add_text("terminator: too many script variables or value too long\n");
                ctx->status = 2;
            } else {
                ctx->status = 0;
            }
            continue;
        }

        char expanded_line[SCRIPT_LINE_CAPACITY];
        if (script_expand_variables(line, ctx, expanded_line, sizeof(expanded_line)) != 0) {
            add_text("terminator: expanded script command is too long\n");
            ctx->status = 2;
            continue;
        }
        char command_copy[SCRIPT_LINE_CAPACITY];
        snprintf(command_copy, sizeof(command_copy), "%s", expanded_line);
        char *command = script_trim(command_copy);
        if (script_keyword(command, "exit")) {
            char *arg = command + 4;
            while (isspace((unsigned char)*arg)) arg++;
            char *endptr = NULL;
            long code = 0;
            if (*arg != '\0') {
                code = strtol(arg, &endptr, 10);
                if (endptr == arg || *script_trim(endptr) != '\0' || code < 0 || code > 255) {
                    add_text("terminator: exit expects a status from 0 to 255\n");
                    ctx->status = 2;
                    continue;
                }
            }
            ctx->exited = 1;
            ctx->exit_status = (int)code;
            ctx->status = (int)code;
            break;
        }
        worker_execute_command(slot, command_copy);
        ctx->status = last_exit_status;
    }
    *exit_status = ctx->exited ? ctx->exit_status : ctx->status;
    return 0;
}

int script_has_extension(const char *path)
{
    if (path == NULL) return 0;
    const char *slash = strrchr(path, '/');
    const char *base = slash != NULL ? slash + 1 : path;
    size_t n = strlen(base);
    return n >= 3 && strcmp(base + n - 3, ".sh") == 0;
}

int script_execute_file(int slot, const char *requested_path,
                               int argument_count, char **arguments)
{
    if (requested_path == NULL || *requested_path == '\0') {
        add_text("terminator: source: missing script filename\n");
        return 2;
    }
    char cwd[PATH_MAX], candidate[PATH_MAX], resolved[PATH_MAX];
    copy_tab_cwd(slot, cwd, sizeof(cwd));
    if (requested_path[0] == '/')
        snprintf(candidate, sizeof(candidate), "%s", requested_path);
    else if (snprintf(candidate, sizeof(candidate), "%s/%s", cwd, requested_path) >= (int)sizeof(candidate)) {
        add_text("terminator: script path is too long\n");
        return 2;
    }
    if (realpath(candidate, resolved) == NULL) {
        char message[PATH_MAX + 256];
        snprintf(message, sizeof(message), "terminator: script %s: %s\n", requested_path, strerror(errno));
        add_text(message);
        return 127;
    }
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        add_text("terminator: script is not a regular file\n");
        return 126;
    }
    FILE *file = fopen(resolved, "r");
    if (file == NULL) {
        char message[PATH_MAX + 256];
        snprintf(message, sizeof(message), "terminator: cannot read script %s: %s\n", requested_path, strerror(errno));
        add_text(message);
        return 126;
    }
    char **lines = calloc(SCRIPT_MAX_LINES, sizeof(*lines));
    if (lines == NULL) { fclose(file); add_text("terminator: script memory allocation failed\n"); return 2; }
    size_t count = 0;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int load_error = 0;
    while ((length = getline(&line, &capacity, file)) != -1) {
        if (count >= SCRIPT_MAX_LINES) { load_error = 1; break; }
        if ((size_t)length >= SCRIPT_LINE_CAPACITY) { load_error = 1; break; }
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        lines[count] = strdup(line);
        if (lines[count] == NULL) { load_error = 1; break; }
        count++;
    }
    free(line);
    fclose(file);
    if (load_error) {
        add_text("terminator: script is too large or memory is exhausted\n");
        for (size_t i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return 2;
    }
    size_t bad_line = 0;
    if (script_validate_structure(lines, count, &bad_line) != 0) {
        char message[128];
        snprintf(message, sizeof(message), "terminator: script syntax error near line %zu\n", bad_line);
        add_text(message);
        for (size_t i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return 2;
    }

    ScriptContext *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        add_text("terminator: script context allocation failed\n");
        for (size_t i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return 2;
    }
    snprintf(ctx->script_path, sizeof(ctx->script_path), "%s", requested_path);
    if (argument_count > SCRIPT_MAX_ARGS) argument_count = SCRIPT_MAX_ARGS;
    for (int i = 0; i < argument_count; i++)
        snprintf(ctx->args[i], sizeof(ctx->args[i]), "%s", arguments[i]);
    ctx->arg_count = argument_count;
    ctx->status = last_exit_status;
    int status = 0;
    (void)script_execute_range(slot, lines, 0, count, ctx, &status);
    status = ctx->exited ? ctx->exit_status : ctx->status;
    for (size_t i = 0; i < count; i++) free(lines[i]);
    free(lines);
    free(ctx);
    last_exit_status = status;
    return status;
}

int script_dispatch_if_requested(int slot, ParsedCommand commands[], int count)
{
    if (count != 1 || commands[0].infile != NULL || commands[0].outfile != NULL ||
        commands[0].argc == 0)
        return 0;
    char **argv = commands[0].argv;
    const char *path = NULL;
    int first_arg = 0;
    if (strcmp(argv[0], "source") == 0) {
        if (argv[1] == NULL) { add_text("source: missing filename\n"); last_exit_status = 2; return 1; }
        path = argv[1]; first_arg = 2;
    } else if (strcmp(argv[0], "bash") == 0 && argv[1] != NULL && script_has_extension(argv[1])) {
        path = argv[1]; first_arg = 2;
    } else if (script_has_extension(argv[0])) {
        path = argv[0]; first_arg = 1;
    } else {
        return 0;
    }
    int argc = 0;
    while (argv[first_arg + argc] != NULL && argc < SCRIPT_MAX_ARGS) argc++;
    last_exit_status = script_execute_file(slot, path, argc, &argv[first_arg]);
    return 1;
}
