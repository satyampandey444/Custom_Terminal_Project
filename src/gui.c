#include "terminator_internal.h"
#include <X11/Xatom.h>

#define MAX_SELECTION_BYTES 60000U

/* Gui module. */

/*
 * Terminal text selection and ICCCM clipboard support.
 * Selection coordinates refer to the output[] line buffer.
 */
static int selection_active;
static int selection_dragging;
static int selection_anchor_line;
static int selection_anchor_col;
static int selection_end_line;
static int selection_end_col;
static char *selection_clipboard;
static Atom clipboard_atom;
static Atom utf8_atom;
static Atom targets_atom;
static Atom clipboard_property;
static Atom paste_selection_atom;
static Atom paste_requested_target;
static int paste_pending;
static int tab_left_edge = 8;
static int tab_left_width = 24;
static int tab_right_edge = 0;
static int tab_right_width = 24;

/* Return the byte offset immediately after the next visually wrapped segment. */
static int wrapped_segment_end(const char *line, int start, int available_width)
{
    int length = (int)strlen(line);
    if (start >= length)
        return length;
    if (available_width < 20)
        available_width = 20;

    int end = start;
    int last_space = -1;
    int segment_width = 0;
    for (int i = start; i < length; i++) {
        int character_width = measured_text_width(line + i, 1);
        if (segment_width + character_width > available_width)
            break;
        segment_width += character_width;
        end = i + 1;
        if (isspace((unsigned char)line[i]))
            last_space = i + 1;
    }

    if (end == start)
        return start + 1;
    if (end < length && last_space > start)
        end = last_space;
    return end;
}

static int total_visual_rows(int available_width)
{
    int rows = 0;
    if (line_count < 1)
        return 0;
    for (int line = 0; line < line_count; line++) {
        int length = (int)strlen(output[line]);
        if (length == 0) {
            rows++;
            continue;
        }
        int start = 0;
        while (start < length) {
            int end = wrapped_segment_end(output[line], start, available_width);
            if (end <= start)
                end = start + 1;
            start = end;
            rows++;
        }
    }
    return rows;
}

static int locate_visual_row(int target_row, int available_width,
                             int *line_out, int *start_out, int *end_out)
{
    int row = 0;
    for (int line = 0; line < line_count; line++) {
        int length = (int)strlen(output[line]);
        if (length == 0) {
            if (row == target_row) {
                *line_out = line;
                *start_out = 0;
                *end_out = 0;
                return 1;
            }
            row++;
            continue;
        }
        int start = 0;
        while (start < length) {
            int end = wrapped_segment_end(output[line], start, available_width);
            if (end <= start)
                end = start + 1;
            if (row == target_row) {
                *line_out = line;
                *start_out = start;
                *end_out = end;
                return 1;
            }
            row++;
            start = end;
        }
    }
    return 0;
}

void refresh_history_search(void)
{
    if (!search_mode)
        return;
    history_search_count = history_search_collect(
        input, history_search_results, MAX_SEARCH_RESULTS);
    if (history_search_count <= 0)
        history_search_index = 0;
    else if (history_search_index >= history_search_count)
        history_search_index = 0;
}

void cancel_history_search(void)
{
    if (!search_mode)
        return;
    snprintf(input, sizeof(input), "%s", history_search_saved_input);
    input_len = strlen(input);
    input_cursor = input_len;
    search_mode = 0;
    history_search_count = 0;
    history_search_index = 0;
}

void scroll_output_by(int rows)
{
    int available_width = window_width - 30;
    int total = total_visual_rows(available_width);
    int maximum = total > 0 ? total - 1 : 0;
    scroll_offset += rows;
    if (scroll_offset < 0)
        scroll_offset = 0;
    if (scroll_offset > maximum)
        scroll_offset = maximum;
    draw_window();
}

void scroll_tabs_by(int direction)
{
    if (direction < 0 && tab_scroll_offset > 0)
        tab_scroll_offset--;
    else if (direction > 0 && tab_scroll_offset < tab_count - 1)
        tab_scroll_offset++;
    draw_window();
}

static void selection_atoms_init(void)
{
    if (display == NULL)
        return;
    if (clipboard_atom == None)
        clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    if (utf8_atom == None)
        utf8_atom = XInternAtom(display, "UTF8_STRING", False);
    if (targets_atom == None)
        targets_atom = XInternAtom(display, "TARGETS", False);
    if (clipboard_property == None)
        clipboard_property = XInternAtom(display, "TERMINATOR_CLIPBOARD", False);
}

static int selection_compare(const int *line_a, const int *col_a,
                             const int *line_b, const int *col_b)
{
    return (*line_a != *line_b || *col_a != *col_b);
}

static int text_column_at_x(const char *line, int x)
{
    if (x <= 15)
        return 0;
    int length = (int)strlen(line);
    int previous_width = 0;
    for (int i = 0; i < length; i++) {
        int next_width = measured_text_width(line, i + 1);
        if (x - 15 < (previous_width + next_width) / 2)
            return i;
        previous_width = next_width;
    }
    return length;
}

static int selection_visible_range(int *first, int *last, int *line_height_out)
{
    int line_height = font != NULL
                    ? font->ascent + font->descent + 10 : 32;
    int prompt_baseline = font != NULL
                        ? window_height - font->descent - 12 : window_height - 12;
    int first_baseline = 48 + (font != NULL ? font->ascent : 18);
    int available_height = prompt_baseline - line_height - first_baseline;
    int visible_rows = available_height > 0 ? available_height / line_height : 1;
    if (visible_rows < 1)
        visible_rows = 1;

    int total = total_visual_rows(window_width - 30);
    int max_scroll = total > 0 ? total - 1 : 0;
    if (scroll_offset > max_scroll)
        scroll_offset = max_scroll;
    int last_row = total - scroll_offset;
    if (last_row < 0)
        last_row = 0;
    if (last_row > total)
        last_row = total;
    int first_row = last_row - visible_rows;
    if (first_row < 0)
        first_row = 0;
    *first = first_row;
    *last = last_row;
    *line_height_out = line_height;
    return first_baseline;
}

static void selection_position_from_xy(int x, int y, int *line, int *column)
{
    int first, last, line_height;
    int first_baseline = selection_visible_range(&first, &last, &line_height);
    int row = (y - first_baseline + line_height / 2) / line_height;
    if (row < 0)
        row = 0;
    int selected_visual_row = first + row;
    if (last > first && selected_visual_row >= last)
        selected_visual_row = last - 1;
    if (selected_visual_row < 0)
        selected_visual_row = 0;

    int segment_start = 0, segment_end = 0, selected_line = 0;
    if (!locate_visual_row(selected_visual_row, window_width - 30,
                           &selected_line, &segment_start, &segment_end)) {
        selected_line = line_count > 0 ? line_count - 1 : 0;
        segment_start = 0;
        segment_end = (int)strlen(output[selected_line]);
    }

    *line = selected_line;
    *column = segment_start + text_column_at_x(output[selected_line] + segment_start, x);
    if (*column > segment_end)
        *column = segment_end;
}

static void selection_get_bounds(int *first_line, int *first_col,
                                 int *last_line, int *last_col)
{
    *first_line = selection_anchor_line;
    *first_col = selection_anchor_col;
    *last_line = selection_end_line;
    *last_col = selection_end_col;
    if (*first_line > *last_line ||
        (*first_line == *last_line && *first_col > *last_col)) {
        int t = *first_line;
        *first_line = *last_line;
        *last_line = t;
        t = *first_col;
        *first_col = *last_col;
        *last_col = t;
    }
}

static char *selection_extract_text(void)
{
    if (!selection_active)
        return NULL;

    int first_line, first_col, last_line, last_col;
    selection_get_bounds(&first_line, &first_col, &last_line, &last_col);
    if (first_line < 0 || last_line < first_line || last_line >= MAX_LINES)
        return NULL;

    size_t capacity = (size_t)(last_line - first_line + 1) *
                      ((size_t)MAX_LINE_LENGTH + 1U) + 1U;
    if (capacity > MAX_SELECTION_BYTES + 1U)
        capacity = MAX_SELECTION_BYTES + 1U;
    char *result = malloc(capacity);
    if (result == NULL)
        return NULL;

    size_t used = 0;
    for (int line = first_line; line <= last_line; line++) {
        size_t length = strnlen(output[line], MAX_LINE_LENGTH);
        size_t begin = line == first_line ? (size_t)first_col : 0U;
        size_t finish = line == last_line ? (size_t)last_col : length;
        if (begin > length)
            begin = length;
        if (finish > length)
            finish = length;
        if (finish > begin && used < capacity - 1U) {
            size_t count = finish - begin;
            if (count > capacity - 1U - used)
                count = capacity - 1U - used;
            memcpy(result + used, output[line] + begin, count);
            used += count;
        }
        if (line != last_line && used < capacity - 1U)
            result[used++] = '\n';
    }
    result[used] = '\0';
    return result;
}

static void selection_refresh_clipboard(void)
{
    char *copy = selection_extract_text();
    if (copy == NULL)
        return;
    free(selection_clipboard);
    selection_clipboard = copy;
}

static void selection_claim_ownership(void)
{
    if (display == NULL || selection_clipboard == NULL)
        return;
    selection_atoms_init();
    XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
    XSetSelectionOwner(display, clipboard_atom, window, CurrentTime);
    XStoreBytes(display, selection_clipboard, (int)strlen(selection_clipboard));
    XFlush(display);
}

void handle_terminal_selection_press(int x, int y, unsigned int button)
{
    if (button != Button1 || y < 42 || y >= window_height - 35)
        return;

    selection_position_from_xy(x, y, &selection_anchor_line, &selection_anchor_col);
    selection_end_line = selection_anchor_line;
    selection_end_col = selection_anchor_col;
    selection_active = 0;
    selection_dragging = 1;

    /* Keep receiving motion/release events even if the pointer leaves the window. */
    (void)XGrabPointer(display, window, False,
                       PointerMotionMask | ButtonReleaseMask,
                       GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

void handle_terminal_selection_motion(int x, int y)
{
    if (!selection_dragging)
        return;
    selection_position_from_xy(x, y, &selection_end_line, &selection_end_col);
    selection_active = selection_compare(
        &selection_anchor_line, &selection_anchor_col,
        &selection_end_line, &selection_end_col);
    draw_window();
}

void handle_terminal_selection_release(int x, int y, unsigned int button)
{
    if (button != Button1 || !selection_dragging)
        return;

    selection_position_from_xy(x, y, &selection_end_line, &selection_end_col);
    selection_active = selection_compare(
        &selection_anchor_line, &selection_anchor_col,
        &selection_end_line, &selection_end_col);
    selection_dragging = 0;
    XUngrabPointer(display, CurrentTime);

    if (selection_active) {
        /* Refresh on every new drag; never serve a stale previous selection. */
        selection_refresh_clipboard();
        selection_claim_ownership();
    }
    draw_window();
}

void copy_terminal_selection(void)
{
    if (!selection_active)
        return;
    selection_refresh_clipboard();
    selection_claim_ownership();
}

static void request_paste_target(Atom selection, Atom target)
{
    if (display == NULL)
        return;
    selection_atoms_init();
    paste_selection_atom = selection;
    paste_requested_target = target;
    paste_pending = 1;
    XConvertSelection(display, selection, target,
                      clipboard_property, window, CurrentTime);
    XFlush(display);
}

void paste_terminal_clipboard(void)
{
    selection_atoms_init();
    request_paste_target(clipboard_atom, utf8_atom);
}

void paste_terminal_primary_selection(void)
{
    request_paste_target(XA_PRIMARY, utf8_atom);
}

static int paste_insert_character(unsigned char c)
{
    if (c == '\t')
        c = ' ';
    if (c < 32 || c == 127)
        return 0;
    if (input_len >= INPUT_CAPACITY - 1) {
        add_text("terminator: pasted input is too long; remaining text ignored\n");
        return -1;
    }
    if (input_cursor > input_len)
        input_cursor = input_len;
    memmove(input + input_cursor + 1, input + input_cursor,
            input_len - input_cursor + 1U);
    input[input_cursor++] = (char)c;
    input_len++;
    history_navigation = -1;
    return 1;
}

static int paste_insert_data(const unsigned char *data, unsigned long length)
{
    if (data == NULL || length == 0)
        return 0;

    if (search_mode) {
        int changed = 0;
        for (unsigned long i = 0; i < length; i++) {
            unsigned char c = data[i];
            if (c == '\r' || c == '\n')
                c = ' ';
            int result = paste_insert_character(c);
            if (result < 0)
                break;
            changed |= result;
        }
        return changed;
    }

    int has_newline = 0;
    for (unsigned long i = 0; i < length; i++) {
        if (data[i] == '\n' || data[i] == '\r') {
            has_newline = 1;
            break;
        }
    }

    /* Single-line paste behaves like normal text insertion; multi-line paste
       is fed through the same parser/continuation path as pressing Enter. */
    if (!has_newline) {
        int changed = 0;
        for (unsigned long i = 0; i < length; i++) {
            int result = paste_insert_character(data[i]);
            if (result < 0)
                break;
            changed |= result;
        }
        return changed;
    }

    int changed = 0;
    int last_was_newline = 0;
    for (unsigned long i = 0; i < length; i++) {
        unsigned char c = data[i];
        if (c == '\r' || c == '\n') {
            /* CRLF is one separator, while lone CR and lone LF also work. */
            if (c == '\r' && i + 1 < length && data[i + 1] == '\n')
                i++;
            submit_command_line();
            changed = 1;
            last_was_newline = 1;
            continue;
        }

        int result = paste_insert_character(c);
        if (result < 0)
            break;
        if (result > 0) {
            changed = 1;
            last_was_newline = 0;
        }
    }

    /* Clipboard selections often omit the final newline. Execute that final
       command too, but do not submit an extra blank command for trailing LF. */
    if (!last_was_newline && input_len > 0) {
        submit_command_line();
        changed = 1;
    }
    return changed;
}

static void paste_try_fallback(void)
{
    if (!paste_pending)
        return;

    if (paste_requested_target == utf8_atom) {
        request_paste_target(paste_selection_atom, XA_STRING);
    } else if (paste_selection_atom == clipboard_atom) {
        request_paste_target(XA_PRIMARY, utf8_atom);
    } else {
        paste_pending = 0;
    }
}

void handle_terminal_clipboard_event(XEvent *event)
{
    if (event == NULL || display == NULL)
        return;
    selection_atoms_init();

    if (event->type == SelectionRequest) {
        XSelectionRequestEvent *request = &event->xselectionrequest;
        XSelectionEvent response;
        memset(&response, 0, sizeof(response));
        response.type = SelectionNotify;
        response.display = request->display;
        response.requestor = request->requestor;
        response.selection = request->selection;
        response.target = request->target;
        response.time = request->time;
        response.property = None;

        if (request->selection == XA_PRIMARY || request->selection == clipboard_atom) {
            Atom property = request->property != None ? request->property : request->target;
            if (request->target == targets_atom) {
                Atom supported[] = {targets_atom, utf8_atom, XA_STRING};
                XChangeProperty(display, request->requestor, property,
                                XA_ATOM, 32, PropModeReplace,
                                (unsigned char *)supported,
                                (int)(sizeof(supported) / sizeof(supported[0])));
                response.property = property;
            } else if (request->target == utf8_atom || request->target == XA_STRING) {
                if (selection_clipboard != NULL) {
                    XChangeProperty(display, request->requestor, property,
                                    request->target, 8, PropModeReplace,
                                    (unsigned char *)selection_clipboard,
                                    (int)strlen(selection_clipboard));
                    response.property = property;
                }
            }
        }

        XSendEvent(display, request->requestor, False, 0, (XEvent *)&response);
        XFlush(display);
    } else if (event->type == SelectionNotify) {
        if (!paste_pending ||
            event->xselection.selection != paste_selection_atom ||
            event->xselection.target != paste_requested_target)
            return;
        if (event->xselection.property == None) {
            paste_try_fallback();
            return;
        }

        Atom actual_type = None;
        int actual_format = 0;
        unsigned long item_count = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        int status = XGetWindowProperty(display, window,
                                        event->xselection.property,
                                        0, (long)INPUT_CAPACITY, True,
                                        AnyPropertyType, &actual_type,
                                        &actual_format, &item_count,
                                        &bytes_after, &data);

        if (status == Success && data != NULL && actual_format == 8 &&
            (actual_type == utf8_atom || actual_type == XA_STRING ||
             actual_type == paste_requested_target)) {
            (void)paste_insert_data(data, item_count);
            paste_pending = 0;
            XFree(data);
            if (search_mode)
                refresh_history_search();
            draw_window();
        } else {
            if (data != NULL)
                XFree(data);
            paste_try_fallback();
        }
    } else if (event->type == SelectionClear) {
        /* The clipboard owner changed. The last local copy remains cached. */
    }
}

void select_all_terminal_output(void)
{
    if (line_count <= 0)
        return;
    selection_anchor_line = 0;
    selection_anchor_col = 0;
    selection_end_line = line_count - 1;
    selection_end_col = (int)strnlen(output[line_count - 1], MAX_LINE_LENGTH);
    selection_active = selection_compare(
        &selection_anchor_line, &selection_anchor_col,
        &selection_end_line, &selection_end_col);
    selection_dragging = 0;
    if (selection_active) {
        selection_refresh_clipboard();
        selection_claim_ownership();
    }
    draw_window();
}

int current_tab_slot(void)
{
    if (worker_context_slot >= 0 && worker_context_slot < MAX_TABS)
        return worker_context_slot;
    if (active_tab < 0 || active_tab >= tab_count)
        return 0;
    return tab_slots[active_tab];
}

void copy_tab_cwd(int slot, char *dest, size_t capacity)
{
    if (capacity == 0)
        return;
    if (slot < 0 || slot >= MAX_TABS) {
        snprintf(dest, capacity, "/");
        return;
    }
    pthread_mutex_lock(&tab_data_mutex);
    snprintf(dest, capacity, "%s",
             tab_states[slot].cwd[0] ? tab_states[slot].cwd : "/");
    pthread_mutex_unlock(&tab_data_mutex);
}

void set_tab_cwd(int slot, const char *cwd)
{
    if (slot < 0 || slot >= MAX_TABS || cwd == NULL)
        return;
    pthread_mutex_lock(&tab_data_mutex);
    snprintf(tab_states[slot].cwd, sizeof(tab_states[slot].cwd), "%s", cwd);
    pthread_mutex_unlock(&tab_data_mutex);
}

void worker_output_append(int slot, const char *text, size_t length)
{
    if (text == NULL || length == 0)
        return;
    if (process_child_mode) {
        while (length > 0) {
            uint32_t chunk = (uint32_t)(length > PROC_MAX_FRAME ? PROC_MAX_FRAME : length);
            if (process_send_frame(PROC_MSG_OUTPUT, text, chunk) != 0)
                return;
            text += chunk;
            length -= chunk;
        }
        return;
    }
    if (slot < 0 || slot >= MAX_TABS)
        return;
    TabWorker *worker = &tab_workers[slot];
    if (!worker->started)
        return;
    pthread_mutex_lock(&worker->mutex);
    if (length >= sizeof(worker->pending_output)) {
        memcpy(worker->pending_output,
               text + length - sizeof(worker->pending_output) + 1,
               sizeof(worker->pending_output) - 1);
        worker->pending_output[sizeof(worker->pending_output) - 1] = '\0';
        worker->pending_length = sizeof(worker->pending_output) - 1;
        worker->output_truncated = 1;
    } else {
        if (worker->pending_length + length >= sizeof(worker->pending_output)) {
            size_t drop = worker->pending_length + length - sizeof(worker->pending_output) + 1;
            memmove(worker->pending_output, worker->pending_output + drop,
                    worker->pending_length - drop);
            worker->pending_length -= drop;
            worker->output_truncated = 1;
        }
        memcpy(worker->pending_output + worker->pending_length, text, length);
        worker->pending_length += length;
        worker->pending_output[worker->pending_length] = '\0';
    }
    pthread_mutex_unlock(&worker->mutex);
}

void worker_output_char(int slot, char c)
{
    worker_output_append(slot, &c, 1);
}

void save_tab_state(int index, int save_cwd)
{
    if (index < 0 || index >= MAX_TABS)
        return;
    TabState *t = &tab_states[index];
    memcpy(t->input, input, sizeof(input));
    t->input_len = input_len;
    t->input_cursor = input_cursor;
    t->history_navigation = history_navigation;
    memcpy(t->history_saved_input, history_saved_input, sizeof(history_saved_input));
    t->scroll_offset = scroll_offset;
    t->search_mode = search_mode;
    memcpy(t->completion_choices, completion_choices, sizeof(completion_choices));
    t->completion_choice_count = completion_choice_count;
    t->completion_selection_mode = completion_selection_mode;
    memcpy(t->completion_choice_input, completion_choice_input, sizeof(completion_choice_input));
    t->completion_choice_len = completion_choice_len;
    t->completion_replace_start = completion_replace_start;
    t->completion_replace_end = completion_replace_end;
    memcpy(t->output, output, sizeof(output));
    t->line_count = line_count;
    memcpy(t->pending_command, pending_command, sizeof(pending_command));
    t->pending_command_len = pending_command_len;
    t->continuation_mode = continuation_mode;
    t->last_exit_status = last_exit_status;
    /* The working directory is stored explicitly per tab. Do not call
       getcwd() here: the process cwd is global and must not overwrite it. */
    (void)save_cwd;
}

void load_tab_state(int index)
{
    if (index < 0 || index >= MAX_TABS)
        return;
    TabState *t = &tab_states[index];
    memcpy(input, t->input, sizeof(input));
    input_len = t->input_len;
    input_cursor = t->input_cursor;
    history_navigation = t->history_navigation;
    memcpy(history_saved_input, t->history_saved_input, sizeof(history_saved_input));
    scroll_offset = t->scroll_offset;
    search_mode = t->search_mode;
    memcpy(completion_choices, t->completion_choices, sizeof(completion_choices));
    completion_choice_count = t->completion_choice_count;
    completion_selection_mode = t->completion_selection_mode;
    memcpy(completion_choice_input, t->completion_choice_input, sizeof(completion_choice_input));
    completion_choice_len = t->completion_choice_len;
    completion_replace_start = t->completion_replace_start;
    completion_replace_end = t->completion_replace_end;
    memcpy(output, t->output, sizeof(output));
    line_count = t->line_count;
    memcpy(pending_command, t->pending_command, sizeof(pending_command));
    pending_command_len = t->pending_command_len;
    continuation_mode = t->continuation_mode;
    pthread_mutex_lock(&tab_data_mutex);
    last_exit_status = t->last_exit_status;
    pthread_mutex_unlock(&tab_data_mutex);
}

void add_char(char c)
{
    if (worker_context_slot >= 0) {
        worker_output_char(worker_context_slot, c);
        return;
    }
    scroll_offset = 0;
    if (c == '\r')
        return;

    if (c == '\n') {
        if (line_count < MAX_LINES) {
            line_count++;
            output[line_count - 1][0] = '\0';
        } else {
            memmove(output, output + 1,
                    sizeof(output[0]) * (MAX_LINES - 1));
            output[MAX_LINES - 1][0] = '\0';
        }
        return;
    }

    int col = (int)strlen(output[line_count - 1]);

    if (col < MAX_LINE_LENGTH - 1) {
        output[line_count - 1][col] = c;
        output[line_count - 1][col + 1] = '\0';
    }
}

void add_text(const char *text)
{
    if (text == NULL)
        return;
    if (process_child_mode) {
        size_t length = strlen(text);
        while (length > 0) {
            uint32_t chunk = (uint32_t)(length > PROC_MAX_FRAME ? PROC_MAX_FRAME : length);
            if (process_send_frame(PROC_MSG_OUTPUT, text, chunk) != 0)
                return;
            text += chunk;
            length -= chunk;
        }
        return;
    }
    while (*text)
        add_char(*text++);
}

int measured_text_width(const char *text, int length)
{
    if (length <= 0)
        return 0;
    if (font != NULL)
        return XTextWidth(font, text, length);
    return length * 10;
}

const char *tab_path_for_display(int visual_index, char *buffer, size_t capacity)
{
    char path[PATH_MAX];
    int slot = (visual_index >= 0 && visual_index < tab_count)
             ? tab_slots[visual_index] : 0;
    copy_tab_cwd(slot, path, sizeof(path));
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0' && strncmp(path, home, strlen(home)) == 0 &&
        (path[strlen(home)] == '/' || path[strlen(home)] == '\0')) {
        snprintf(buffer, capacity, "~%s", path + strlen(home));
        return buffer;
    }
    snprintf(buffer, capacity, "%s", path);
    return buffer;
}

int resolve_tab_directory(const char *requested,
                                 char resolved[PATH_MAX])
{
    if (requested == NULL || requested[0] == '\0') {
        requested = getenv("HOME");
        if (requested == NULL || requested[0] == '\0')
            return -1;
    }

    char expanded[PATH_MAX];
    const char *home = getenv("HOME");
    if (requested[0] == '~' &&
        (requested[1] == '\0' || requested[1] == '/')) {
        if (home == NULL || home[0] == '\0')
            return -1;
        int n = snprintf(expanded, sizeof(expanded), "%s%s",
                         home, requested + 1);
        if (n < 0 || (size_t)n >= sizeof(expanded)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        requested = expanded;
    }

    char candidate[PATH_MAX];
    if (requested[0] == '/') {
        int n = snprintf(candidate, sizeof(candidate), "%s", requested);
        if (n < 0 || (size_t)n >= sizeof(candidate)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        char base[PATH_MAX];
        copy_tab_cwd(current_tab_slot(), base, sizeof(base));
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", base, requested);
        if (n < 0 || (size_t)n >= sizeof(candidate)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    if (realpath(candidate, resolved) == NULL)
        return -1;

    struct stat st;
    if (stat(resolved, &st) == -1)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int enter_active_tab_directory(void)
{
    /* This helper is called in the child after fork(). Do not acquire a
       pthread mutex here: in a multithreaded parent a lock may be inherited
       in the locked state. The child sees a private snapshot of this memory. */
    char cwd[PATH_MAX];
    int slot = current_tab_slot();
    snprintf(cwd, sizeof(cwd), "%s",
             tab_states[slot].cwd[0] ? tab_states[slot].cwd : "/");
    if (chdir(cwd) == -1) {
        dprintf(STDERR_FILENO, "terminator: cannot enter tab directory %s: %s\n",
                cwd, strerror(errno));
        return -1;
    }
    return 0;
}

void ensure_active_tab_visible(void)
{
    int right_zone = window_width - 105;
    if (right_zone < 120)
        right_zone = 120;
    if (tab_count <= 0) {
        tab_scroll_offset = 0;
        return;
    }
    if (tab_scroll_offset < 0)
        tab_scroll_offset = 0;
    if (tab_scroll_offset >= tab_count)
        tab_scroll_offset = tab_count - 1;
    if (active_tab < 0)
        active_tab = 0;
    if (active_tab >= tab_count)
        active_tab = tab_count - 1;
    if (active_tab < tab_scroll_offset)
        tab_scroll_offset = active_tab;

    while (tab_scroll_offset < active_tab) {
        int active_right = 38;
        for (int i = tab_scroll_offset; i <= active_tab; i++) {
            char path[PATH_MAX], label[160];
            const char *shown = tab_path_for_display(i, path, sizeof(path));
            const char *leaf = strrchr(shown, '/');
            leaf = (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : shown;
            if (strlen(leaf) > 18)
                leaf += strlen(leaf) - 18;
            if (i == active_tab)
                snprintf(label, sizeof(label), "{%d:%.18s}", i + 1, leaf);
            else
                snprintf(label, sizeof(label), "[%d:%.18s]", i + 1, leaf);
            active_right += measured_text_width(label, (int)strlen(label)) + 22;
        }
        if (active_right <= right_zone)
            break;
        tab_scroll_offset++;
    }
}

void draw_tab_bar(void)
{
    int baseline = font != NULL ? font->ascent + 8 : 25;
    int right_zone = window_width - 105;
    if (right_zone < 120)
        right_zone = 120;

    tab_left_edge = 8;
    tab_left_width = 24;
    tab_right_edge = right_zone + 5;
    tab_right_width = 24;

    const char left_label[] = "<";
    const char right_label[] = ">";
    const char plus_label[] = "[+]";
    int plus_x = window_width - 48;
    if (plus_x < right_zone + 32)
        plus_x = right_zone + 32;

    XDrawString(display, window, gc, tab_left_edge + 5, baseline,
                left_label, 1);
    XDrawString(display, window, gc, tab_right_edge + 5, baseline,
                right_label, 1);
    XDrawString(display, window, gc, plus_x, baseline,
                plus_label, 3);
    plus_hit_left = plus_x - 5;
    plus_hit_right = plus_x + measured_text_width(plus_label, 3) + 5;

    if (tab_scroll_offset < 0)
        tab_scroll_offset = 0;
    if (tab_scroll_offset >= tab_count)
        tab_scroll_offset = tab_count > 0 ? tab_count - 1 : 0;

    for (int i = 0; i < MAX_TABS + 1; i++) {
        tab_hit_left[i] = -1;
        tab_hit_right[i] = -1;
    }
    int x = 38;
    for (int i = tab_scroll_offset; i < tab_count; i++) {
        char path[PATH_MAX];
        char label[160];
        const char *shown_path = tab_path_for_display(i, path, sizeof(path));
        const char *leaf = strrchr(shown_path, '/');
        leaf = (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : shown_path;
        if (strlen(leaf) > 18)
            leaf += strlen(leaf) - 18;
        if (i == active_tab)
            snprintf(label, sizeof(label), "{%d:%.18s}", i + 1, leaf);
        else
            snprintf(label, sizeof(label), "[%d:%.18s]", i + 1, leaf);
        int width = measured_text_width(label, (int)strlen(label));
        if (x + width + 8 > right_zone)
            break;
        tab_hit_left[i] = x - 4;
        tab_hit_right[i] = x + width + 8;
        XDrawString(display, window, gc, x, baseline, label, (int)strlen(label));
        if (i == active_tab)
            XDrawLine(display, window, gc, x, baseline + 4, x + width, baseline + 4);
        x += width + 22;
    }
    for (int i = 0; i < MAX_TABS + 1; i++) {
        if (i < tab_scroll_offset || i >= tab_count ||
            tab_hit_right[i] <= 0) {
            tab_hit_left[i] = -1;
            tab_hit_right[i] = -1;
        }
    }
}

void draw_window(void)
{
    if (display == NULL || window == 0 || gc == 0)
        return;

    XClearWindow(display, window);
    draw_tab_bar();

    int line_height = font != NULL
                    ? font->ascent + font->descent + 10 : 32;
    int prompt_baseline = font != NULL
                        ? window_height - font->descent - 12 : window_height - 12;
    int first_baseline = 48 + (font != NULL ? font->ascent : 18);
    int available_height = prompt_baseline - line_height - first_baseline;
    int visible_rows = available_height > 0 ? available_height / line_height : 1;
    if (visible_rows < 1)
        visible_rows = 1;

    int available_width = window_width - 30;
    if (available_width < 20)
        available_width = 20;

    if (search_mode) {
        const char *heading = "History matches (Up/Down: select, Enter: load, Esc: cancel)";
        XDrawString(display, window, gc, 15, first_baseline,
                    heading, (int)strlen(heading));
        int list_rows = visible_rows - 1;
        if (list_rows < 1)
            list_rows = 1;
        int first_match = 0;
        if (history_search_index >= list_rows)
            first_match = history_search_index - list_rows + 1;
        for (int row = 0; row < list_rows; row++) {
            int match_index = first_match + row;
            if (match_index >= history_search_count)
                break;
            const char *match = history_search_results[match_index];
            int length = (int)strlen(match);
            while (length > 0 && measured_text_width(match, length) > available_width)
                length--;
            int baseline = first_baseline + (row + 1) * line_height;
            if (match_index == history_search_index) {
                int width = measured_text_width(match, length);
                XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
                XFillRectangle(display, window, gc, 12,
                               baseline - (font != NULL ? font->ascent : 18),
                               (unsigned int)(width + 8),
                               (unsigned int)(font != NULL ? font->ascent + font->descent + 4 : 24));
                XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
                XDrawString(display, window, gc, 16, baseline, match, length);
                XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
            } else {
                XDrawString(display, window, gc, 16, baseline, match, length);
            }
        }
        if (history_search_count == 0) {
            const char *none = input_len == 0 ? "Type to search command history..."
                                               : "No matching command (substring must be longer than 2 characters)";
            int baseline = first_baseline + line_height;
            XDrawString(display, window, gc, 16, baseline, none, (int)strlen(none));
        }
    } else {
        int total = total_visual_rows(available_width);
        int max_scroll = total > 0 ? total - 1 : 0;
        if (scroll_offset > max_scroll)
            scroll_offset = max_scroll;
        int end_row = total - scroll_offset;
        if (end_row < 0)
            end_row = 0;
        if (end_row > total)
            end_row = total;
        int start_row = end_row - visible_rows;
        if (start_row < 0)
            start_row = 0;

        int sel_first_line = 0, sel_first_col = 0;
        int sel_last_line = -1, sel_last_col = 0;
        if (selection_active)
            selection_get_bounds(&sel_first_line, &sel_first_col,
                                 &sel_last_line, &sel_last_col);

        int visual_row = 0;
        for (int line = 0; line < line_count; line++) {
            int length = (int)strlen(output[line]);
            int segment_start = 0;
            do {
                int segment_end = length == 0 ? 0
                    : wrapped_segment_end(output[line], segment_start, available_width);
                if (segment_end <= segment_start && length > 0)
                    segment_end = segment_start + 1;

                if (visual_row >= start_row && visual_row < end_row) {
                    int baseline = first_baseline + (visual_row - start_row) * line_height;
                    const char *segment = output[line] + segment_start;
                    int segment_length = segment_end - segment_start;
                    int highlight_begin = -1;
                    int highlight_end = -1;
                    if (selection_active && line >= sel_first_line && line <= sel_last_line) {
                        int selected_begin = (line == sel_first_line) ? sel_first_col : 0;
                        int selected_end = (line == sel_last_line) ? sel_last_col : length;
                        if (selected_begin < segment_end && selected_end > segment_start) {
                            highlight_begin = selected_begin > segment_start
                                            ? selected_begin - segment_start : 0;
                            highlight_end = selected_end < segment_end
                                          ? selected_end - segment_start : segment_length;
                        }
                    }

                    if (highlight_begin >= 0 && highlight_end > highlight_begin) {
                        int x1 = 15 + measured_text_width(segment, highlight_begin);
                        int x2 = 15 + measured_text_width(segment, highlight_end);
                        XFillRectangle(display, window, gc, x1,
                                       baseline - (font != NULL ? font->ascent : 18),
                                       (unsigned int)(x2 - x1),
                                       (unsigned int)(font != NULL ? font->ascent + font->descent + 2 : 22));
                        XDrawString(display, window, gc, 15, baseline,
                                    segment, highlight_begin);
                        XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
                        XDrawString(display, window, gc, x1, baseline,
                                    segment + highlight_begin,
                                    highlight_end - highlight_begin);
                        XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
                        XDrawString(display, window, gc, x2, baseline,
                                    segment + highlight_end,
                                    segment_length - highlight_end);
                    } else {
                        XDrawString(display, window, gc, 15, baseline,
                                    segment, segment_length);
                    }
                }

                visual_row++;
                if (length == 0)
                    break;
                segment_start = segment_end;
            } while (segment_start < length);
        }
    }

    const char *prefix = completion_selection_mode
                       ? "Select match number (Enter to choose): "
                       : (search_mode ? "reverse-i-search: "
                          : (continuation_mode ? "> " : "user@terminator> "));
    const char *display_input = completion_selection_mode ? completion_choice_input : input;
    size_t display_len = completion_selection_mode ? completion_choice_len : input_len;
    size_t display_cursor = completion_selection_mode ? completion_choice_len : input_cursor;
    int prefix_width = measured_text_width(prefix, (int)strlen(prefix));
    int input_width = window_width - 30 - prefix_width;
    if (input_width < 20)
        input_width = 20;

    size_t visible_start = 0;
    while (visible_start < display_cursor &&
           measured_text_width(display_input + visible_start,
                               (int)(display_cursor - visible_start)) > input_width)
        visible_start++;

    char prompt[INPUT_CAPACITY + 96];
    int prompt_len = snprintf(prompt, sizeof(prompt), "%s%.*s", prefix,
                              (int)(display_len - visible_start),
                              display_input + visible_start);
    if (prompt_len < 0)
        prompt_len = 0;
    if ((size_t)prompt_len >= sizeof(prompt))
        prompt_len = (int)sizeof(prompt) - 1;
    XDrawString(display, window, gc, 15, prompt_baseline, prompt, prompt_len);

    int cursor_offset = (int)(display_cursor - visible_start);
    int cursor_x = 15 + prefix_width +
                   measured_text_width(display_input + visible_start, cursor_offset);
    int cursor_top = prompt_baseline - (font != NULL ? font->ascent : 18);
    int cursor_bottom = prompt_baseline + (font != NULL ? font->descent : 4);
    XDrawLine(display, window, gc, cursor_x, cursor_top, cursor_x, cursor_bottom);
    XFlush(display);
}

void switch_to_tab(int index)
{
    if (index < 0 || index >= tab_count || index == active_tab)
        return;
    if (search_mode)
        cancel_history_search();
    save_tab_state(current_tab_slot(), 1);
    selection_active = 0;
    selection_dragging = 0;
    active_tab = index;
    ensure_active_tab_visible();
    load_tab_state(current_tab_slot());
    draw_window();
}

void create_tab(void)
{
    if (search_mode)
        cancel_history_search();
    if (tab_count >= MAX_TABS) {
        add_text("terminator: maximum number of tabs reached\n");
        draw_window();
        return;
    }

    int new_slot = -1;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!slot_used[i]) {
            new_slot = i;
            break;
        }
    }
    if (new_slot < 0) {
        add_text("terminator: no free tab slot\n");
        draw_window();
        return;
    }

    int previous_slot = current_tab_slot();
    save_tab_state(previous_slot, 1);
    selection_active = 0;
    selection_dragging = 0;
    memset(&tab_states[new_slot], 0, sizeof(tab_states[new_slot]));
    char cwd[PATH_MAX];
    copy_tab_cwd(previous_slot, cwd, sizeof(cwd));
    snprintf(tab_states[new_slot].cwd, sizeof(tab_states[new_slot].cwd), "%s", cwd);
    tab_states[new_slot].line_count = 1;
    tab_states[new_slot].output[0][0] = '\0';
    tab_states[new_slot].history_navigation = -1;
    tab_states[new_slot].last_exit_status = 0;
    slot_used[new_slot] = 1;
    tab_slots[tab_count] = new_slot;
    tab_count++;
    active_tab = tab_count - 1;
    ensure_active_tab_visible();

    if (worker_start(new_slot) != 0) {
        tab_count--;
        active_tab = tab_count - 1;
        slot_used[new_slot] = 0;
        memset(&tab_states[new_slot], 0, sizeof(tab_states[new_slot]));
        load_tab_state(current_tab_slot());
        add_text("terminator: could not start tab worker thread\n");
        draw_window();
        return;
    }

    load_tab_state(new_slot);
    add_text("New tab opened. History is shared; input and directory are per-tab.\n");
    draw_window();
}

void close_active_tab(void)
{
    if (search_mode)
        cancel_history_search();
    if (tab_count <= 1) {
        add_text("terminator: cannot close the last tab with this shortcut; use exit\n");
        draw_window();
        return;
    }
    save_tab_state(current_tab_slot(), 1);
    int closing_visual = active_tab;
    int closing_slot = current_tab_slot();

    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].owner_tab == closing_slot && jobs[i].is_foreground) {
            add_text("terminator: cannot close a tab while its foreground job is active\n");
            draw_window();
            return;
        }
    }

    /* Stop only this tab's worker/process group. Other tabs keep running. */
    worker_stop(closing_slot);
    selection_active = 0;
    selection_dragging = 0;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active || jobs[i].owner_tab != closing_slot)
            continue;
        if (jobs[i].pgid > 0) {
            (void)kill(-jobs[i].pgid, SIGKILL);
            for (int p = 0; p < jobs[i].proc_count; p++) {
                if (jobs[i].pids[p] > 0) {
                    int status;
                    while (waitpid(jobs[i].pids[p], &status, 0) == -1 && errno == EINTR) {}
                }
            }
        }
        release_job(i);
    }

    for (int i = closing_visual; i < tab_count - 1; i++)
        tab_slots[i] = tab_slots[i + 1];
    tab_slots[tab_count - 1] = 0;
    tab_count--;
    slot_used[closing_slot] = 0;
    memset(&tab_states[closing_slot], 0, sizeof(tab_states[closing_slot]));

    active_tab = (closing_visual >= tab_count) ? tab_count - 1 : closing_visual;
    ensure_active_tab_visible();
    load_tab_state(current_tab_slot());
    draw_window();
}

void handle_tab_click(int x, int y)
{
    if (y > 42 || y < 0)
        return;
    if (x >= tab_left_edge && x <= tab_left_edge + tab_left_width) {
        scroll_tabs_by(-1);
        return;
    }
    if (x >= tab_right_edge && x <= tab_right_edge + tab_right_width) {
        scroll_tabs_by(1);
        return;
    }
    if (x >= plus_hit_left && x <= plus_hit_right) {
        create_tab();
        return;
    }
    for (int i = 0; i < tab_count; i++) {
        if (tab_hit_left[i] >= 0 && x >= tab_hit_left[i] && x <= tab_hit_right[i]) {
            switch_to_tab(i);
            return;
        }
    }
}

void append_text_to_tab(int slot, const char *text)
{
    if (text == NULL)
        return;
    if (slot < 0 || slot >= MAX_TABS || !slot_used[slot])
        return;
    int previous_slot = current_tab_slot();
    if (slot == previous_slot) {
        add_text(text);
        return;
    }
    save_tab_state(previous_slot, 1);
    load_tab_state(slot);
    add_text(text);
    save_tab_state(slot, 0);
    load_tab_state(previous_slot);
}

void clear_output_for_slot(int slot)
{
    if (slot == current_tab_slot()) {
        line_count = 1;
        output[0][0] = '\0';
        scroll_offset = 0;
        selection_active = 0;
        selection_dragging = 0;
    } else if (slot >= 0 && slot < MAX_TABS) {
        tab_states[slot].line_count = 1;
        tab_states[slot].output[0][0] = '\0';
        tab_states[slot].scroll_offset = 0;
    }
}
