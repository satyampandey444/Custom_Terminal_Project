#include "terminator_internal.h"

/* Mouse selection and X11 clipboard helpers implemented in gui.c. */
void handle_terminal_selection_press(int x, int y, unsigned int button);
void handle_terminal_selection_motion(int x, int y);
void handle_terminal_selection_release(int x, int y, unsigned int button);
void copy_terminal_selection(void);
void paste_terminal_clipboard(void);
void paste_terminal_primary_selection(void);
void select_all_terminal_output(void);
void handle_terminal_clipboard_event(XEvent *event);

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "useProcess=True") == 0)
            use_process_mode = 1;
        else {
            fprintf(stderr, "Usage: %s [useProcess=True]\n", argv[0]);
            return 2;
        }
    }

    if (use_process_mode && process_shared_history_init() != 0) {
        perror("terminator: shared history initialization failed");
        return 1;
    }

    /* A closed tab-process pipe should report EPIPE, not terminate the GUI. */
    (void)signal(SIGPIPE, SIG_IGN);
    display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr, "Cannot open X11 display\n");
        return 1;
    }

    int screen = DefaultScreen(display);

    window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        100, 100, WIDTH, HEIGHT,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    XStoreName(display, window, "Terminator");

    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                  ButtonReleaseMask | PointerMotionMask);

    gc = XCreateGC(display, window, 0, NULL);

    /*
     * Load a larger, bold font. The second choice is a fallback for
     * X servers that do not have the first font installed.
     */
    font = XLoadQueryFont(
        display, "-misc-fixed-bold-r-normal--24-*-*-*-*-*-*-*"
    );

    if (font == NULL)
        font = XLoadQueryFont(display, "10x20bold");

    if (font != NULL) {
        XSetFont(display, gc, font->fid);
    } else {
        fprintf(stderr,
                "Warning: could not load the preferred X11 fonts; "
                "using the default font.\n");
    }

    XMapWindow(display, window);

    output[0][0] = '\0';
    history_load();
    add_text(use_process_mode
             ? "Terminator: Process Mode enabled (separate shell process per tab).\n"
             : "Terminator: Thread Mode enabled (worker thread per tab).\n");
    add_text("Command history enabled.\n");
    add_text("Use Up/Down for history; Ctrl+R searches history.\n");
    add_text("Arrows edit; Tab completes filenames; PageUp/PageDown scroll.\n");
    add_text("Quotes and $(...) can span lines; Esc cancels continuation.\n");
    add_text("Scripts: ./script.sh, bash script.sh, source script.sh; if/elif/else/fi.\n");
    add_text("Job control: command &, jobs, Ctrl+C, Ctrl+Z, fg, bg.\n");
    add_text("Tabs: click a tab or [+]; Ctrl+Shift+T new; Ctrl+Shift+W close; Ctrl+PageUp/Down switch.\n");
    (void)getcwd(tab_states[0].cwd, sizeof(tab_states[0].cwd));
    tab_slots[0] = 0;
    slot_used[0] = 1;
    tab_states[0].history_navigation = -1;
    save_tab_state(0, 1);
    for (int i = 0; i < MAX_TABS; i++) {
        process_tabs[i].command_fd = -1;
        process_tabs[i].output_fd = -1;
    }
    if (worker_start(0) != 0) {
        add_text(use_process_mode
                 ? "terminator: could not start initial tab process\n"
                 : "terminator: could not start initial tab worker thread\n");
        running = 0;
    }

    while (running) {
        worker_drain_output();
        fd_set readfds;
        FD_ZERO(&readfds);
        int xfd = ConnectionNumber(display);
        FD_SET(xfd, &readfds);
        int maxfd = xfd;
        if (use_process_mode) {
            for (int slot = 0; slot < MAX_TABS; slot++) {
                if (process_tabs[slot].started && process_tabs[slot].output_fd >= 0) {
                    FD_SET(process_tabs[slot].output_fd, &readfds);
                    if (process_tabs[slot].output_fd > maxfd)
                        maxfd = process_tabs[slot].output_fd;
                }
            }
        }
        int fd_snapshot[MAX_JOBS];
        for (int i = 0; i < MAX_JOBS; i++)
            fd_snapshot[i] = jobs[i].active ? jobs[i].output_fd : -1;
        add_background_job_fds(&readfds, &maxfd, -1);
        struct timeval timeout = {0, 200000};
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0 && errno != EINTR) {
            add_text("terminator: select failed in event loop\n");
            draw_window();
            continue;
        }
        if (use_process_mode && ready > 0)
            process_drain_output();

        if (ready > 0 && FD_ISSET(xfd, &readfds)) {
          while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);

        /*
         * SelectionRequest/SelectionNotify/SelectionClear are delivered
         * through the normal X11 event queue and are handled by gui.c.
         */
        if (event.type == SelectionRequest ||
            event.type == SelectionNotify ||
            event.type == SelectionClear) {
            handle_terminal_clipboard_event(&event);
        }

        if (event.type == Expose) {
            draw_window();
        }

        if (event.type == ButtonPress) {
            if (event.xbutton.button == Button1) {
                handle_tab_click(event.xbutton.x, event.xbutton.y);
                handle_terminal_selection_press(event.xbutton.x,
                                                event.xbutton.y,
                                                event.xbutton.button);
            } else if (event.xbutton.button == Button2) {
                /* Middle-click pastes the X11 PRIMARY selection. */
                paste_terminal_primary_selection();
            }
        }

        if (event.type == MotionNotify) {
            handle_terminal_selection_motion(event.xmotion.x,
                                             event.xmotion.y);
        }

        if (event.type == ButtonRelease) {
            handle_terminal_selection_release(event.xbutton.x,
                                              event.xbutton.y,
                                              event.xbutton.button);
        }

        if (event.type == KeyPress) {
            char chars[64];
            KeySym key;
            int n = XLookupString(&event.xkey, chars, sizeof(chars), &key, NULL);
            int control = (event.xkey.state & ControlMask) != 0;
            int shift = (event.xkey.state & ShiftMask) != 0;
            int changed = 0;

            /*
             * Use Ctrl+Shift+C/V for terminal copy/paste so Ctrl+C keeps
             * its existing foreground-job interrupt behaviour.
             */
            if ((control && shift && (key == XK_c || key == XK_C)) ||
                (control && key == XK_Insert)) {
                copy_terminal_selection();
                continue;
            }
            if ((control && shift && (key == XK_v || key == XK_V)) ||
                (shift && key == XK_Insert)) {
                paste_terminal_clipboard();
                continue;
            }
            if (control && shift && (key == XK_a || key == XK_A)) {
                select_all_terminal_output();
                continue;
            }

            if (handle_tab_hotkey(key, event.xkey.state))
                continue;

            if (completion_selection_mode) {
                handle_completion_selection(key, chars, n);
                continue;
            }

            if (control) {
                switch (key) {
                    case XK_a:
                    case XK_A:
                        input_cursor = 0;
                        changed = 1;
                        break;
                    case XK_e:
                    case XK_E:
                        input_cursor = input_len;
                        changed = 1;
                        break;
                    case XK_u:
                    case XK_U:
                        history_navigation = -1;
                        if (input_cursor > 0) {
                            memmove(input, input + input_cursor,
                                    input_len - input_cursor + 1);
                            input_len -= input_cursor;
                            input_cursor = 0;
                        }
                        changed = 1;
                        break;
                    case XK_k:
                    case XK_K:
                        history_navigation = -1;
                        input_len = input_cursor;
                        input[input_len] = '\0';
                        changed = 1;
                        break;
                    case XK_w:
                    case XK_W: {
                        history_navigation = -1;
                        size_t new_cursor = input_cursor;
                        while (new_cursor > 0 && input[new_cursor - 1] == ' ')
                            new_cursor--;
                        while (new_cursor > 0 && input[new_cursor - 1] != ' ' &&
                               input[new_cursor - 1] != '\t')
                            new_cursor--;
                        if (new_cursor < input_cursor) {
                            memmove(input + new_cursor, input + input_cursor,
                                    input_len - input_cursor + 1);
                            input_len -= input_cursor - new_cursor;
                            input_cursor = new_cursor;
                        }
                        changed = 1;
                        break;
                    }
                    case XK_l:
                    case XK_L:
                        line_count = 1;
                        scroll_offset = 0;
                        output[0][0] = '\0';
                        changed = 1;
                        break;
                    case XK_c:
                    case XK_C: {
                        pid_t pgid = worker_active_pgid(current_tab_slot());
                        if (pgid > 0) {
                            (void)kill(-pgid, SIGINT);
                            add_text("^C\n");
                            changed = 1;
                        }
                        break;
                    }
                    case XK_z:
                    case XK_Z: {
                        if (use_process_mode) {
                            int slot = current_tab_slot();
                            pid_t pgid = worker_active_pgid(slot);
                            pid_t shell_pgid = (slot >= 0 && slot < MAX_TABS &&
                                                process_tabs[slot].started)
                                                   ? process_tabs[slot].pid : 0;

                            /*
                             * In Process Mode, multiWatch runs inside the tab
                             * shell process and publishes that shell's own
                             * process group as active. Suspending that group
                             * would stop the shell itself, leaving later input
                             * queued forever. multiWatch is stopped with Ctrl+C;
                             * Ctrl+Z is reserved for foreground child jobs.
                             */
                            if (pgid > 0 && pgid != shell_pgid) {
                                if (kill(-pgid, SIGTSTP) == 0) {
                                    add_text("^Z\n");
                                    changed = 1;
                                }
                            } else if (pgid > 0 && pgid == shell_pgid) {
                                add_text("multiWatch cannot be suspended; press Ctrl+C to stop it\n");
                                changed = 1;
                            }
                        }
                        break;
                    }
                    case XK_d:
                    case XK_D:
                        history_navigation = -1;
                        if (continuation_mode && input_len == 0) {
                            pending_command[0] = '\0';
                            pending_command_len = 0;
                            continuation_mode = 0;
                            clear_current_input();
                            add_text("terminator: multiline command cancelled\n");
                        } else if (input_len == 0 && !search_mode) {
                            running = 0;
                        } else if (input_cursor < input_len) {
                            memmove(input + input_cursor, input + input_cursor + 1,
                                    input_len - input_cursor);
                            input_len--;
                        }
                        changed = 1;
                        break;
                    case XK_r:
                    case XK_R:
                        search_mode = 1;
                        input_len = 0;
                        input_cursor = 0;
                        input[0] = '\0';
                        history_navigation = -1;
                        changed = 1;
                        break;
                    default:
                        break;
                }
            } else if (key == XK_Return || key == XK_KP_Enter) {
                if (search_mode) {
                    input[input_len] = '\0';
                    add_text("Enter search term: ");
                    add_text(input);
                    add_char('\n');
                    history_search(input);
                    search_mode = 0;
                    clear_current_input();
                } else {
                    submit_command_line();
                }
                changed = 1;
            } else if (key == XK_Escape && continuation_mode) {
                pending_command[0] = '\0';
                pending_command_len = 0;
                continuation_mode = 0;
                clear_current_input();
                add_text("terminator: multiline command cancelled\n");
                changed = 1;
            } else if (key == XK_Page_Up) {
                int step = 10;
                scroll_offset += step;
                if (scroll_offset > line_count - 1)
                    scroll_offset = line_count - 1;
                if (scroll_offset < 0)
                    scroll_offset = 0;
                changed = 1;
            } else if (key == XK_Page_Down) {
                scroll_offset -= 10;
                if (scroll_offset < 0)
                    scroll_offset = 0;
                changed = 1;
            } else if (key == XK_Tab) {
                complete_filename();
                changed = 1;
            } else if (key == XK_Left) {
                if (input_cursor > 0)
                    input_cursor--;
                changed = 1;
            } else if (key == XK_Right) {
                if (input_cursor < input_len)
                    input_cursor++;
                changed = 1;
            } else if (key == XK_Home) {
                input_cursor = 0;
                changed = 1;
            } else if (key == XK_End) {
                input_cursor = input_len;
                changed = 1;
            } else if (key == XK_Up) {
                history_up();
                changed = 1;
            } else if (key == XK_Down) {
                history_down();
                changed = 1;
            } else if (key == XK_BackSpace) {
                history_navigation = -1;
                if (input_cursor > 0) {
                    memmove(input + input_cursor - 1, input + input_cursor,
                            input_len - input_cursor + 1);
                    input_len--;
                    input_cursor--;
                }
                changed = 1;
            } else if (key == XK_Delete) {
                history_navigation = -1;
                if (input_cursor < input_len) {
                    memmove(input + input_cursor, input + input_cursor + 1,
                            input_len - input_cursor);
                    input_len--;
                }
                changed = 1;
            } else if (n > 0) {
                history_navigation = -1;
                for (int i = 0; i < n; i++) {
                    unsigned char c = (unsigned char)chars[i];
                    if (c >= 32 && c < 127 && input_len < INPUT_CAPACITY - 1) {
                        memmove(input + input_cursor + 1, input + input_cursor,
                                input_len - input_cursor + 1);
                        input[input_cursor] = (char)c;
                        input_len++;
                        input_cursor++;
                    }
                }
                changed = 1;
            }

            if (changed)
                draw_window();
          }
        }
        }

        if (ready > 0)
            service_background_job_fds(&readfds, -1, fd_snapshot);
        refresh_background_jobs(-1);
        draw_window();
    }

    /* Stop every per-tab command worker before releasing shared state. */
    for (int slot = 0; slot < MAX_TABS; slot++)
        worker_stop(slot);

    /* Terminate any remaining background or stopped jobs when the shell exits. */
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].pgid > 0) {
            (void)kill(-jobs[i].pgid, SIGHUP);
            (void)kill(-jobs[i].pgid, SIGCONT);
        }
        if (jobs[i].active)
            release_job(i);
    }

    for (int i = 0; i < history_count; i++)
        free(history_entries[(history_start + i) % MAX_HISTORY]);

    if (process_shared_history != NULL) {
        (void)sem_destroy(&process_shared_history->semaphore);
        (void)munmap(process_shared_history, sizeof(*process_shared_history));
        process_shared_history = NULL;
    }

    if (font != NULL)
        XFreeFont(display, font);

    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}

