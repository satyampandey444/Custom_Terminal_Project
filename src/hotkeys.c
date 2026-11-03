#include "terminator_internal.h"

/* Hotkeys module. */

static HotkeyConfig hotkey_config;
static int hotkey_config_loaded = 0;

int parse_hotkey_binding(const char *text, HotkeyBinding *out)
{
    if (text == NULL || out == NULL)
        return -1;

    char buffer[128];
    if (strlen(text) >= sizeof(buffer))
        return -1;
    snprintf(buffer, sizeof(buffer), "%s", text);

    unsigned int modifiers = 0;
    char *saveptr = NULL;
    char *part = strtok_r(buffer, "+", &saveptr);
    char *key_name = NULL;
    while (part != NULL) {
        while (isspace((unsigned char)*part))
            part++;
        size_t length = strlen(part);
        while (length > 0 && isspace((unsigned char)part[length - 1]))
            part[--length] = '\0';

        if (strcasecmp(part, "ctrl") == 0 || strcasecmp(part, "control") == 0)
            modifiers |= ControlMask;
        else if (strcasecmp(part, "shift") == 0)
            modifiers |= ShiftMask;
        else if (strcasecmp(part, "alt") == 0)
            modifiers |= Mod1Mask;
        else if (*part != '\0')
            key_name = part;
        part = strtok_r(NULL, "+", &saveptr);
    }

    if (key_name == NULL || *key_name == '\0')
        return -1;

    KeySym key = XStringToKeysym(key_name);
    if (key == NoSymbol && strlen(key_name) == 1)
        key = (KeySym)(unsigned char)key_name[0];
    if (key == NoSymbol)
        return -1;

    out->key = key;
    out->modifiers = modifiers;
    return 0;
}

void load_hotkey_config(void)
{
    if (hotkey_config_loaded)
        return;
    hotkey_config_loaded = 1;

    (void)parse_hotkey_binding("Ctrl+Shift+T", &hotkey_config.new_tab);
    (void)parse_hotkey_binding("Ctrl+Shift+W", &hotkey_config.close_tab);
    (void)parse_hotkey_binding("Ctrl+Page_Down", &hotkey_config.next_tab);
    (void)parse_hotkey_binding("Ctrl+Page_Up", &hotkey_config.previous_tab);

    FILE *file = fopen("terminator.conf", "r");
    if (file == NULL)
        return;

    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        char *comment = strchr(line, '#');
        if (comment != NULL)
            *comment = '\0';
        char *equals = strchr(line, '=');
        if (equals == NULL)
            continue;
        *equals++ = '\0';

        char *name = line;
        while (isspace((unsigned char)*name))
            name++;
        char *end = name + strlen(name);
        while (end > name && isspace((unsigned char)end[-1]))
            *--end = '\0';

        while (isspace((unsigned char)*equals))
            equals++;
        end = equals + strlen(equals);
        while (end > equals && isspace((unsigned char)end[-1]))
            *--end = '\0';

        HotkeyBinding binding;
        if (parse_hotkey_binding(equals, &binding) != 0)
            continue;

        if (strcmp(name, "new_tab") == 0)
            hotkey_config.new_tab = binding;
        else if (strcmp(name, "close_tab") == 0)
            hotkey_config.close_tab = binding;
        else if (strcmp(name, "next_tab") == 0)
            hotkey_config.next_tab = binding;
        else if (strcmp(name, "previous_tab") == 0)
            hotkey_config.previous_tab = binding;
    }
    fclose(file);
}

int hotkey_matches(KeySym key, unsigned int state,
                          const HotkeyBinding *binding)
{
    if (binding == NULL || binding->key == NoSymbol)
        return 0;
    unsigned int relevant = state & (ControlMask | ShiftMask | Mod1Mask);
    return key == binding->key && relevant == binding->modifiers;
}

int handle_tab_hotkey(KeySym key, unsigned int state)
{
    load_hotkey_config();

    if (hotkey_matches(key, state, &hotkey_config.new_tab)) {
        create_tab();
        return 1;
    }
    if (hotkey_matches(key, state, &hotkey_config.close_tab)) {
        close_active_tab();
        return 1;
    }
    if (hotkey_matches(key, state, &hotkey_config.next_tab)) {
        switch_to_tab((active_tab + 1) % tab_count);
        return 1;
    }
    if (hotkey_matches(key, state, &hotkey_config.previous_tab)) {
        switch_to_tab((active_tab - 1 + tab_count) % tab_count);
        return 1;
    }

    int control = (state & ControlMask) != 0;
    int shift = (state & ShiftMask) != 0;
    if (control && key == XK_Tab) {
        switch_to_tab((active_tab + (shift ? -1 : 1) + tab_count) % tab_count);
        return 1;
    }
    return 0;
}
