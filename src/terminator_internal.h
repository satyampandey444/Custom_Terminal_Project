#ifndef TERMINATOR_INTERNAL_H
#define TERMINATOR_INTERNAL_H

/* Feature-test macros must precede all system headers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <stdint.h>

/* Feature-test macros are defined once, before any system/X11 header. */
#define WIDTH 1200
#define HEIGHT 800
#define MIN_WINDOW_WIDTH 700
#define MIN_WINDOW_HEIGHT 450
#define MAX_SEARCH_RESULTS 64
#define INPUT_CAPACITY 4096
#define MAX_COMMAND_SIZE 16384
#define MAX_SUBSTITUTION_OUTPUT 8192
#define MAX_EXPANSION_DEPTH 16
#define MAX_LINES 1200
#define MAX_LINE_LENGTH 1024
#define MAX_COMMANDS 16
#define MAX_ARGS 64
#define MAX_TOKENS 256
#define MAX_TOKEN_LEN 256
#define MAX_HISTORY 10000
#define HISTORY_DISPLAY_LIMIT 1000
#define MAX_COMPLETIONS 128
#define MAX_JOBS 64
#define MAX_TABS 8
#define WORKER_COMMAND_QUEUE 8
#define WORKER_OUTPUT_CAPACITY (256 * 1024)
#define PROC_MAX_FRAME 65536U
#define PROC_RX_CAPACITY (2U * (PROC_MAX_FRAME + sizeof(uint32_t) * 2U))
#define SAM_ALPHABET 128
#define MAX_WATCH_COMMANDS 16
#define WATCH_OUTPUT_CAPACITY 32768
#define WATCH_INTERVAL_SECONDS 2
#define WATCH_SEPARATOR "----------------------------------------------------\n"
#define SCRIPT_MAX_LINES 4096
#define SCRIPT_MAX_VARIABLES 64
#define SCRIPT_MAX_ARGS 64
#define SCRIPT_MAX_IF_DEPTH 64
#define SCRIPT_LINE_CAPACITY MAX_COMMAND_SIZE

/* Shared data types used across modules. */

typedef struct {
    char text[MAX_TOKEN_LEN];
    int is_operator;
} Token;

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
} ParsedCommand;

enum { JOB_UNUSED = 0, JOB_RUNNING, JOB_STOPPED, JOB_DONE };

typedef struct {
    int active;
    int id;
    int owner_tab;
    pid_t pgid;
    pid_t pids[MAX_COMMANDS];
    int proc_state[MAX_COMMANDS];
    int proc_count;
    int state;
    int output_fd;
    int completion_notified;
    int is_foreground;
    int last_status;
    char command[INPUT_CAPACITY];
} Job;

typedef struct {
    char input[INPUT_CAPACITY];
    size_t input_len;
    size_t input_cursor;
    int history_navigation;
    char history_saved_input[INPUT_CAPACITY];
    int scroll_offset;
    int search_mode;
    char completion_choices[MAX_COMPLETIONS][INPUT_CAPACITY];
    int completion_choice_count;
    int completion_selection_mode;
    char completion_choice_input[16];
    size_t completion_choice_len;
    size_t completion_replace_start;
    size_t completion_replace_end;
    char output[MAX_LINES][MAX_LINE_LENGTH];
    int line_count;
    char pending_command[MAX_COMMAND_SIZE];
    size_t pending_command_len;
    int continuation_mode;
    int last_exit_status;
    char cwd[PATH_MAX];
} TabState;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int started;
    int shutdown;
    int busy;
    char commands[WORKER_COMMAND_QUEUE][MAX_COMMAND_SIZE];
    int queue_head;
    int queue_count;
    char pending_output[WORKER_OUTPUT_CAPACITY];
    size_t pending_length;
    int output_truncated;
    int clear_requested;
    pid_t active_pgid;
} TabWorker;

enum {
    PROC_MSG_OUTPUT = 1,
    PROC_MSG_CLEAR = 2,
    PROC_MSG_CWD = 3,
    PROC_MSG_PGID = 4,
    PROC_MSG_EXIT = 5
};

typedef struct {
    uint32_t type;
    uint32_t length;
} ProcessMessageHeader;

typedef struct {
    sem_t semaphore;
    int count;
    int start;
    char entries[MAX_HISTORY][INPUT_CAPACITY];
} SharedHistory;

typedef struct {
    pid_t pid;
    int command_fd;
    int output_fd;
    int started;
    pid_t active_pgid;
    unsigned char incoming[PROC_RX_CAPACITY];
    size_t incoming_length;
} ProcessTab;

typedef struct {
    int next[SAM_ALPHABET];
    int link;
    int length;
} SuffixState;

typedef struct {
    KeySym key;
    unsigned int modifiers;
} HotkeyBinding;

typedef struct {
    HotkeyBinding new_tab;
    HotkeyBinding close_tab;
    HotkeyBinding next_tab;
    HotkeyBinding previous_tab;
} HotkeyConfig;

typedef struct {
    char command[INPUT_CAPACITY];
    pid_t pid;                 /* per-command worker process and process-group id */
    int output_fd;
    int cycle_active;
    int pipe_eof;
    int worker_done;
    int output_truncated;
    time_t started_at;
    time_t next_start_at;
    char output[WATCH_OUTPUT_CAPACITY];
    size_t output_length;
} WatchCommand;

typedef struct {
    char name[128];
    char value[INPUT_CAPACITY];
} ScriptVariable;

typedef struct {
    ScriptVariable variables[SCRIPT_MAX_VARIABLES];
    int variable_count;
    char script_path[PATH_MAX];
    char args[SCRIPT_MAX_ARGS][INPUT_CAPACITY];
    int arg_count;
    int status;
    int exited;
    int exit_status;
} ScriptContext;

typedef struct {
    size_t header;
    size_t body_start;
    size_t body_end;
    int is_else;
    char condition[SCRIPT_LINE_CAPACITY];
} ScriptBranch;

/* Shared state, defined once in state.c. */
extern Display *display;
extern Window window;
extern GC gc;
extern XFontStruct *font;
extern int window_width;
extern int window_height;
extern char input[INPUT_CAPACITY];
extern size_t input_len;
extern size_t input_cursor;
extern char *history_entries[MAX_HISTORY];
extern int history_count;
extern int history_start;
extern char history_path[PATH_MAX];
extern int history_navigation;
extern char history_saved_input[INPUT_CAPACITY];
extern pthread_mutex_t history_mutex;
extern int scroll_offset;
extern int search_mode;
extern char history_search_saved_input[INPUT_CAPACITY];
extern char history_search_results[MAX_SEARCH_RESULTS][INPUT_CAPACITY];
extern int history_search_count;
extern int history_search_index;
extern char completion_choices[MAX_COMPLETIONS][INPUT_CAPACITY];
extern int completion_choice_count;
extern int completion_selection_mode;
extern char completion_choice_input[16];
extern size_t completion_choice_len;
extern size_t completion_replace_start;
extern size_t completion_replace_end;
extern char output[MAX_LINES][MAX_LINE_LENGTH];
extern int line_count;
extern int running;
extern char pending_command[MAX_COMMAND_SIZE];
extern size_t pending_command_len;
extern int continuation_mode;
extern __thread int last_exit_status;
extern TabState tab_states[MAX_TABS];
extern int tab_count;
extern int active_tab;
extern int tab_slots[MAX_TABS];
extern int slot_used[MAX_TABS];
extern pthread_mutex_t tab_data_mutex;
extern __thread int worker_context_slot;
extern int use_process_mode;
extern int process_child_mode;
extern int process_output_fd;
extern SharedHistory *process_shared_history;
extern ProcessTab process_tabs[MAX_TABS];
extern TabWorker tab_workers[MAX_TABS];
extern int tab_hit_left[MAX_TABS + 1];
extern int tab_hit_right[MAX_TABS + 1];
extern int plus_hit_left;
extern int plus_hit_right;
extern int tab_scroll_offset;
extern Job jobs[MAX_JOBS];
extern int next_job_id;
extern int requested_background;
extern char current_job_command[INPUT_CAPACITY];

/* Module interfaces. Functions are declared here so responsibilities can be split across .c files. */
int current_tab_slot(void);
void copy_tab_cwd(int slot, char *dest, size_t capacity);
void set_tab_cwd(int slot, const char *cwd);
void worker_output_append(int slot, const char *text, size_t length);
void worker_output_char(int slot, char c);
void save_tab_state(int index, int save_cwd);
void load_tab_state(int index);
void add_char(char c);
void add_text(const char *text);
int measured_text_width(const char *text, int length);
const char *tab_path_for_display(int visual_index, char *buffer, size_t capacity);
int resolve_tab_directory(const char *requested,
                                 char resolved[PATH_MAX]);
int enter_active_tab_directory(void);
void draw_tab_bar(void);
void ensure_active_tab_visible(void);
void draw_window(void);
const char *history_at(int logical_index);
int process_shared_history_init(void);
void process_shared_history_append(const char *command);
void history_store_memory(const char *command);
int history_write_encoded(FILE *file, const char *command);
void history_decode_encoded(char *line);
int history_rewrite_file(void);
void history_load(void);
int is_blank_command(const char *command);
void history_record_unlocked(const char *command);
void history_record(const char *command);
void show_history_unlocked(void);
void show_history(void);
void load_history_line(int logical_index);
void history_up(void);
void history_down(void);
void suffix_state_init(SuffixState *state);
SuffixState *build_suffix_automaton(const char *query,
                                           int query_length,
                                           int *state_count_out);
int suffix_automaton_match(const SuffixState *states,
                                  const char *text);
void history_search_unlocked(const char *term);
void history_search(const char *term);
int history_search_collect(const char *term, char results[][INPUT_CAPACITY], int capacity);
int tokenize(const char *line, Token tokens[MAX_TOKENS], int *count);
int parse_command(
    Token tokens[MAX_TOKENS],
    int token_count,
    ParsedCommand commands[MAX_COMMANDS],
    int *command_count);
int execute_pipeline_internal(ParsedCommand commands[], int count,
                                     char *capture, size_t capture_capacity);
int reserve_job(const char *command, int foreground);
void release_job(int index);
void switch_to_tab(int index);
void create_tab(void);
void close_active_tab(void);
int parse_hotkey_binding(const char *text, HotkeyBinding *out);
void load_hotkey_config(void);
int hotkey_matches(KeySym key, unsigned int state,
                          const HotkeyBinding *binding);
int handle_tab_hotkey(KeySym key, unsigned int state);
void handle_tab_click(int x, int y);
void scroll_output_by(int rows);
void scroll_tabs_by(int direction);
void refresh_history_search(void);
void cancel_history_search(void);

/* Terminal mouse selection and X11 clipboard support (implemented in gui.c). */
void handle_terminal_selection_press(int x, int y, unsigned int button);
void handle_terminal_selection_motion(int x, int y);
void handle_terminal_selection_release(int x, int y, unsigned int button);
void copy_terminal_selection(void);
void paste_terminal_clipboard(void);
void handle_terminal_clipboard_event(XEvent *event);
void append_text_to_tab(int slot, const char *text);
int job_refresh_status(Job *job);
void read_job_output(Job *job);
void add_background_job_fds(fd_set *readfds, int *maxfd,
                                   int skip_index);
void service_background_job_fds(fd_set *readfds, int skip_index,
                                       const int *snapshot_fds);
void refresh_background_jobs(int skip_index);
void handle_waiting_x_events(int job_index);
void wait_foreground_job(int index);
int launch_interactive_pipeline(ParsedCommand commands[], int count,
                                       int background, const char *command_line);
int parse_job_id(const char *spec);
void show_jobs(void);
void continue_job_in_background(const char *spec);
void bring_job_to_foreground(const char *spec);
int strip_background_operator(char *command);
void execute_pipeline(ParsedCommand commands[], int count);
int is_completion_delimiter(char c);
int compare_completion_strings(const void *left, const void *right);
int replace_input_range(size_t start, size_t end, const char *replacement);
void cancel_completion_selection(void);
void apply_completion_choice(int index);
void complete_filename(void);
void handle_completion_selection(KeySym key, const char *chars, int n);
int append_char_bounded(char *dest, size_t capacity, size_t *used, char c);
int append_expansion_value(char *dest, size_t capacity, size_t *used,
                                  const char *value, int quote_context);
const char *find_substitution_end(const char *source);
const char *find_backtick_end(const char *source);
int execute_captured_command(const char *command, char *result,
                                    size_t result_capacity, int depth);
int expand_shell_text(const char *source, char *dest, size_t capacity,
                             int depth);
int command_needs_continuation(const char *command, int *backslash_join);
void clear_current_input(void);
void submit_command_line(void);
int parse_multiwatch_invocation(const char *line,
                                       char commands[MAX_WATCH_COMMANDS][INPUT_CAPACITY],
                                       int *command_count);
int execute_pipeline_to_fd(ParsedCommand commands[], int count, int target_fd);
int execute_watch_command(const char *raw_command, int target_fd);
int start_watch_cycle(WatchCommand *watch);
void emit_watch_piece(WatchCommand *watch, const char *data, size_t length);
void finish_watch_cycle(WatchCommand *watch);
void terminate_watch_commands(WatchCommand watches[], int count);
int run_multiwatch(char commands[MAX_WATCH_COMMANDS][INPUT_CAPACITY], int count);
void process_multiwatch_sigint_handler(int signo);
int run_multiwatch_process_mode(int slot,
                                      char commands[MAX_WATCH_COMMANDS][INPUT_CAPACITY],
                                      int count);
void execute_command(char *command);
int process_write_all(int fd, const void *data, size_t length);
int process_read_all(int fd, void *data, size_t length);
int process_send_frame(uint32_t type, const void *data, uint32_t length);
void process_show_shared_history_child(void);
int process_child_wait_foreground_job(int index);
void process_child_dispatch_command(int slot, char *command);
void process_child_service_jobs(int command_fd);
int process_start(int slot);
int process_submit(int slot, const char *command);
void clear_output_for_slot(int slot);
void close_tab_for_slot(int slot);
void process_stop(int slot);
void process_handle_message(int slot, const ProcessMessageHeader *header,
                                   const unsigned char *payload);
void process_drain_output(void);
int worker_start(int slot);
int worker_submit(int slot, const char *command);
int worker_busy(int slot);
pid_t worker_active_pgid(int slot);
void worker_set_active_pgid(int slot, pid_t pgid);
void worker_request_clear(int slot);
void worker_stop(int slot);
void worker_drain_output(void);
int should_run_on_gui_thread(const char *command);
char *script_trim(char *text);
void script_remove_comment(char *line);
int script_keyword(const char *line, const char *keyword);
int script_has_then(const char *line);
int script_line_is_assignment(const char *line, char *name, size_t name_cap,
                                     char *value, size_t value_cap);
const char *script_get_variable(const ScriptContext *ctx, const char *name);
int script_set_variable(ScriptContext *ctx, const char *name, const char *value);
int script_append(char *dest, size_t cap, size_t *used, const char *text, size_t n);
int script_expand_variables(const char *line, ScriptContext *ctx,
                                   char *dest, size_t capacity);
int script_eval_test(char *condition, ScriptContext *ctx);
int script_is_if(const char *line);
int script_is_elif(const char *line);
int script_is_else(const char *line);
int script_is_fi(const char *line);
int script_is_then(const char *line);
int script_validate_structure(char **lines, size_t count, size_t *bad_line);
int script_find_then_line(char **lines, size_t count, size_t from, size_t *body_start);
int script_extract_condition(const char *line, const char *keyword,
                                    char *condition, size_t capacity,
                                    int *inline_then);
int script_execute_if(int slot, char **lines, size_t count, size_t *index,
                             ScriptContext *ctx, int *exit_status);
int script_execute_range(int slot, char **lines, size_t start, size_t end,
                                ScriptContext *ctx, int *exit_status);
int script_has_extension(const char *path);
int script_execute_file(int slot, const char *requested_path,
                               int argument_count, char **arguments);
int script_dispatch_if_requested(int slot, ParsedCommand commands[], int count);
void worker_execute_command(int slot, char *command);
void *tab_worker_main(void *argument);
void dispatch_command(char *command);
int main(int argc, char **argv);

#endif /* TERMINATOR_INTERNAL_H */

