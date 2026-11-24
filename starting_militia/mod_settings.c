/* Native layout and input for Settings > Mods. */
typedef unsigned char Byte;
typedef unsigned short Wide;
typedef unsigned int UInt;
typedef unsigned long long Size;

enum {
    MODS_TAB = 9, OPEN_MODS = 200, FIRST_FIELD = 201, SAVE_SETTINGS = 208,
    FIELD_COUNT = 7, ALL_MAPS = 3, COOP = 4, MAP_IDS = 5, MOD_ENABLED = 6,
    INPUT_CAPACITY = 4096, CHANCE_CAPACITY = 48, DISPLAY_LIMIT = 21,
    EDIT_INVALID = 1, SAVE_DONE, SAVE_FAILED,
    WIDGET_BUTTON = 0, WIDGET_TOGGLE = 1,
};
typedef struct {
    const char *data;
    Size length;
} Text;

typedef struct {
    float x1, y1, x2, y2;
    Byte kind, action;
    Byte padding[6];
    Text label;
    Byte toggle_on;
    Byte reserved[7];
    float rgba[4];
    Byte details[5];
    Byte disabled;
    Byte tail[26];
} Widget;

typedef struct {
    Widget *items;
    Size count, capacity;
} Widgets;

typedef struct {
    void *saving;
    float left;
    Byte initialized, editing, changed, overlong, status;
    char chances[3][CHANCE_CAPACITY];
    Byte toggles[FIELD_COUNT];
    char map_ids[INPUT_CAPACITY];
    char draft[INPUT_CAPACITY];
    char display[FIELD_COUNT][48];
} ModSettings;

_Static_assert(sizeof(Widget) == 0x60 && __builtin_offsetof(Widget, toggle_on) == 0x28 &&
               __builtin_offsetof(Widget, disabled) == 0x45,
               "Stock settings widget layout");
_Static_assert(sizeof(ModSettings) <= 0x2400, "Launcher settings state allocation");

extern Widgets settings_widgets;
extern Byte settings_tab, settings_visible;
extern const Byte settings_game_files_action;
extern const Byte settings_layout_site[];
extern const char mod_initial_settings[];
extern const Wide mod_settings_command[];
extern ModSettings mod_settings_menu;

extern void grow_toolbar(Widgets *, Size stride, Size alignment, Size capacity, const void *site, void *ctx);
extern void set_text_position(float x, float y);
extern void set_text_rgba(UInt r, UInt g, UInt b, UInt a);
extern void draw_ui_text(const Text *, const void *arguments, void *ctx);
extern const Text string_format;
extern float ui_text_position[2];
extern UInt ui_text_color;
extern const float ui_pixels_to_tiles;

typedef struct {
    UInt size;
    Byte reserved[100];
} StartupInfo;
typedef struct {
    void *process, *thread;
    UInt process_id, thread_id;
} ProcessInfo;

__declspec(dllimport) int CreateProcessW(const Wide *, Wide *, void *, void *, int, UInt,
                                        void *, const Wide *, StartupInfo *, ProcessInfo *);
__declspec(dllimport) int GetExitCodeProcess(void *, UInt *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *, UInt flags, Size bytes);
__declspec(dllimport) int HeapFree(void *, UInt flags, void *);
static void copy_bytes(void *out, const void *source, Size count)
{
    Byte *destination = out;
    const Byte *bytes = source;
    for (Size i = 0; i < count; i++)
        destination[i] = bytes[i];
}

static void clear_bytes(void *out, Size count)
{
    Byte *bytes = out;
    for (Size i = 0; i < count; i++)
        bytes[i] = 0;
}

static const char field_labels[FIELD_COUNT][25] = {
    "Residential chance (%)", "Commercial chance (%)", "Elsewhere chance (%)",
    "Allow all maps", "Enable co-op", "Extra U.S. map IDs", "Enable mod",
};

static int field_is_toggle(unsigned field)
{
    return field == MOD_ENABLED || field == ALL_MAPS || field == COOP;
}

static float field_y(unsigned field)
{
    return 130 + 46 * (field == MOD_ENABLED ? 0 : field + 1);
}

static Size length(const char *text)
{
    const char *end = text;
    while (*end)
        end++;
    return (Size)(end - text);
}

static Text as_text(const char *text)
{
    return (Text){text, length(text)};
}

static const char *setting_value(unsigned field)
{
    const ModSettings *menu = &mod_settings_menu;
    if (field < 3)
        return menu->chances[field];
    if (field == MAP_IDS)
        return menu->map_ids;
    return menu->toggles[field] ? "true" : "false";
}

static void initialize_values(void)
{
    ModSettings *menu = &mod_settings_menu;
    if (menu->initialized)
        return;
    const char *value = mod_initial_settings;
    for (unsigned field = 0; field < FIELD_COUNT; field++) {
        const Size size = length(value) + 1;
        if (field_is_toggle(field))
            menu->toggles[field] = *value == 't';
        else if (size > (field == MAP_IDS ? INPUT_CAPACITY : CHANCE_CAPACITY))
            menu->overlong |= 1u << field;
        else
            copy_bytes(field == MAP_IDS ? menu->map_ids : menu->chances[field], value, size);
        value += size;
    }
    menu->initialized = 1;
}

static void save_settings(void)
{
    ModSettings *menu = &mod_settings_menu;
    const Wide *base = mod_settings_command;
    Size size = 1;
    while (*base++)
        size++;
    for (unsigned field = 0; field < FIELD_COUNT; field++)
        size += 4 + ((menu->changed & (1u << field)) ? length(setting_value(field)) : 0);
    if (size > 32767) {
        menu->status = SAVE_FAILED;
        return;
    }
    void *heap = GetProcessHeap();
    Wide *command = HeapAlloc(heap, 0, size * sizeof(Wide));
    if (!command) {
        menu->status = SAVE_FAILED;
        return;
    }
    Wide *end = command;
    for (const Wide *p = mod_settings_command; *p; p++)
        *end++ = *p;
    for (unsigned field = 0; field < FIELD_COUNT; field++) {
        const int changed = (menu->changed & (1u << field)) != 0;
        *end++ = ' ';
        *end++ = '"';
        *end++ = changed ? '+' : '-';
        if (changed)
            for (const char *p = setting_value(field); *p; p++)
                *end++ = (Byte)*p;
        *end++ = '"';
    }
    *end = 0;

    _Alignas(8) StartupInfo startup;
    clear_bytes(&startup, sizeof(startup));
    startup.size = sizeof(startup);
    ProcessInfo process;
    const UInt create_no_window = 0x08000000;
    if (CreateProcessW(0, command, 0, 0, 0, create_no_window, 0, 0, &startup, &process)) {
        CloseHandle(process.thread);
        menu->saving = process.process;
    } else {
        menu->status = SAVE_FAILED;
    }
    HeapFree(heap, 0, command);
}

static void check_save_status(void)
{
    ModSettings *menu = &mod_settings_menu;
    if (!menu->saving)
        return;
    UInt exit_code;
    const int polled = GetExitCodeProcess(menu->saving, &exit_code);
    if (polled && exit_code == 259) // STILL_ACTIVE
        return;
    CloseHandle(menu->saving);
    menu->saving = 0;
    menu->status = polled && !exit_code ? SAVE_DONE : SAVE_FAILED;
    if (menu->status == SAVE_DONE)
        menu->changed = 0;
}

static int map_id_character(unsigned c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
}

static int valid_map_ids(const char *text)
{
    while (*text) {
        while (*text == ' ' || *text == ',')
            text++;
        unsigned size = 0;
        while (map_id_character((Byte)*text)) {
            text++;
            size++;
        }
        while (*text == ' ')
            text++;
        if (size > 80 || (*text && *text != ','))
            return 0;
    }
    return 1;
}

static int valid_chance(const char *text)
{
    unsigned whole = 0, digits = 0;
    int decimal = 0, fraction = 0;
    for (; *text; text++) {
        if (*text == '.' && !decimal) {
            decimal = 1;
            continue;
        }
        if (*text < '0' || *text > '9')
            return 0;
        digits++;
        if (decimal)
            fraction |= *text != '0';
        else if ((whole = 10 * whole + *text - '0') > 100)
            return 0;
    }
    return digits && (whole < 100 || !fraction);
}

static int accept_edit(void)
{
    ModSettings *menu = &mod_settings_menu;
    if (!menu->editing)
        return 1;
    const unsigned field = menu->editing - 1;
    const char *old_value = setting_value(field);
    Size compared = 0;
    while (old_value[compared] && old_value[compared] == menu->draft[compared])
        compared++;
    if (old_value[compared] != menu->draft[compared]) {
        if (!(field == MAP_IDS ? valid_map_ids(menu->draft) : valid_chance(menu->draft))) {
            menu->status = EDIT_INVALID;
            return 0;
        }
        copy_bytes(field == MAP_IDS ? menu->map_ids : menu->chances[field], menu->draft, length(menu->draft) + 1);
        menu->changed |= 1u << field;
        menu->status = 0;
    } else if (menu->status == EDIT_INVALID) {
        menu->status = 0;
    }
    menu->editing = 0;
    return 1;
}

void handle_mod_setting_action(unsigned action)
{
    ModSettings *menu = &mod_settings_menu;
    if (action == OPEN_MODS) {
        settings_tab = MODS_TAB;
        initialize_values();
        return;
    }
    if (settings_tab != MODS_TAB || menu->saving)
        return;
    const unsigned field = action - FIRST_FIELD;
    if (field + 1 == menu->editing && menu->editing)
        return;
    if (!accept_edit())
        return;
    if (action == SAVE_SETTINGS) {
        if (menu->changed) {
            menu->status = 0;
            save_settings();
        }
    } else if (field_is_toggle(field)) {
        menu->toggles[field] = !menu->toggles[field];
        menu->changed |= 1u << field;
        menu->status = 0;
    } else if (!(menu->overlong & (1u << field))) {
        const char *value = setting_value(field);
        copy_bytes(menu->draft, value, length(value) + 1);
        menu->editing = (Byte)(field + 1);
    }
}

int handle_mod_setting_message(unsigned message, unsigned key)
{
    ModSettings *menu = &mod_settings_menu;
    if (!settings_visible || settings_tab != MODS_TAB || !menu->editing ||
        message < 0x100 || message > 0x102)
        return 0;
    if (message != 0x102) // Swallow the game's normal key shortcuts while editing.
        return 1;
    const unsigned field = menu->editing - 1;
    const Size size = length(menu->draft);
    if (key == 27) {
        menu->editing = 0;
        menu->status = 0;
    } else if (key == 13) {
        accept_edit();
    } else if (key == 1 || key == 8) { // Ctrl+A clears the field. Backspace removes a character.
        menu->draft[key == 1 || !size ? 0 : size - 1] = 0;
        menu->status = 0;
    } else {
        const int acceptable = field == MAP_IDS ? map_id_character(key) || key == ',' || key == ' '
                                               : (key >= '0' && key <= '9') || key == '.';
        if (acceptable && size + 1 < (field == MAP_IDS ? INPUT_CAPACITY : CHANCE_CAPACITY)) {
            menu->draft[size] = (char)key;
            menu->draft[size + 1] = 0;
            menu->status = 0;
        }
    }
    return 1;
}

static Widget *button(float x, float y, float width, unsigned action, const char *label, int disabled)
{
    Widget *item = &settings_widgets.items[settings_widgets.count++];
    clear_bytes(item, sizeof(*item));
    item->x1 = x;
    item->y1 = y;
    item->x2 = x + width;
    item->y2 = y + 32;
    item->kind = WIDGET_BUTTON;
    item->action = (Byte)action;
    item->label = as_text(label);
    item->disabled = (Byte)disabled;
    item->rgba[0] = 0.30f;
    item->rgba[1] = 0.32f;
    item->rgba[2] = 0.38f;
    item->rgba[3] = 1;
    return item;
}

static const char *display_value(unsigned field)
{
    ModSettings *menu = &mod_settings_menu;
    if (menu->overlong & (1u << field))
        return "Set in settings.ini";
    const int editing = menu->editing == field + 1;
    const char *value = editing ? menu->draft : setting_value(field);
    const Size size = length(value);
    if (!editing && !size)
        return "None";
    char *text = menu->display[field];
    Size position = 0;
    if (editing && size > DISPLAY_LIMIT) {
        text[position++] = '.';
        text[position++] = '.';
        text[position++] = '.';
        value += size - DISPLAY_LIMIT;
    }
    for (Size i = 0; i < DISPLAY_LIMIT && value[i]; i++)
        text[position++] = value[i];
    if (!editing && size > DISPLAY_LIMIT)
        for (int i = 0; i < 3; i++)
            text[position++] = '.';
    if (editing)
        text[position++] = '_';
    text[position] = 0;
    return text;
}

void build_mod_settings_widgets(void *ctx, float left)
{
    ModSettings *menu = &mod_settings_menu;
    const int open = settings_tab == MODS_TAB;
    if (!open) {
        menu->editing = 0;
        if (menu->status == EDIT_INVALID)
            menu->status = 0;
    }
    check_save_status();
    menu->left = left;
    Size game_files = settings_widgets.count;
    for (Size i = 0; i < settings_widgets.count; i++) {
        const Widget *entry = &settings_widgets.items[i];
        if (entry->kind == WIDGET_BUTTON && entry->action == settings_game_files_action) {
            game_files = i;
            break;
        }
    }
    if (game_files == settings_widgets.count)
        return;
    const Size needed = settings_widgets.count + (open ? 2 + FIELD_COUNT : 1);
    if (settings_widgets.capacity < needed)
        grow_toolbar(&settings_widgets, sizeof(Widget), 8, needed + 8, settings_layout_site, ctx);
    if (settings_widgets.capacity < needed)
        return;

    // The stock tabs share a width. Insert Mods after Game Files and shift
    // later tab rows by one line while keeping the stock Back control.
    Widget *items = settings_widgets.items;
    for (Size i = settings_widgets.count; i > game_files + 1; i--) {
        copy_bytes(&items[i], &items[i - 1], sizeof(Widget));
        if (items[i].kind == WIDGET_BUTTON && items[i].action > settings_game_files_action) {
            items[i].y1 += 42;
            items[i].y2 += 42;
        }
    }
    Widget *tab = &items[game_files + 1];
    copy_bytes(tab, &items[game_files], sizeof(Widget));
    tab->action = OPEN_MODS;
    tab->label = as_text("MODS");
    tab->y1 += 42;
    tab->y2 += 42;
    tab->rgba[0] = open ? 0.92f : 0.30f;
    tab->rgba[1] = open ? 0.95f : 0.32f;
    tab->rgba[2] = open ? 0.31f : 0.38f;
    settings_widgets.count++;
    if (!open)
        return;

    initialize_values();
    for (unsigned i = 0; i < FIELD_COUNT; i++) {
        const int toggle = field_is_toggle(i);
        Widget *item = button(left + 370, field_y(i), 430, FIRST_FIELD + i,
                              toggle ? "" : display_value(i), menu->saving || (menu->overlong & (1u << i)));
        if (toggle) {
            item->kind = WIDGET_TOGGLE;
            item->toggle_on = menu->toggles[i];
        }
    }
    button(left, 476, 215, SAVE_SETTINGS, "SAVE CHANGES", menu->saving || (!menu->changed && !menu->editing));
}

static void draw_text(float x, float y, const char *label, void *ctx)
{
    struct {
        const Text *value;
        Size type;
    } argument;
    struct {
        const void *items;
        Size count;
    } arguments = {&argument, 1};
    const Text value = as_text(label);
    argument.value = &value;
    argument.type = 0x0e285d333f62babfULL;

    ui_text_position[0] = x * ui_pixels_to_tiles;
    ui_text_position[1] = y * ui_pixels_to_tiles;
    set_text_position(ui_text_position[0], ui_text_position[1]);
    draw_ui_text(&string_format, &arguments, ctx);
}

void draw_mod_settings_labels(void *ctx)
{
    const ModSettings *menu = &mod_settings_menu;
    const float x = menu->left;
    const float previous_x = ui_text_position[0], previous_y = ui_text_position[1];
    const UInt previous_color = ui_text_color;
    set_text_rgba(238, 238, 243, 255);
    ui_text_color = 0xfff3eeee;
    draw_text(x, 81, "Armed Citizens Mod", ctx);
    for (unsigned i = 0; i < FIELD_COUNT; i++)
        draw_text(x, field_y(i) + 6, field_labels[i], ctx);

    const char *hint = "Save changes, then relaunch the mod.";
    if (menu->saving)
        hint = "Saving...";
    else if (menu->status == EDIT_INVALID)
        hint = menu->editing == MAP_IDS + 1 ? "Separate map IDs with commas. Omit .map." : "Enter a number from 0 to 100.";
    else if (menu->editing)
        hint = "Enter accepts. Esc cancels. Ctrl+A clears.";
    else if (menu->status == SAVE_DONE)
        hint = "Saved. Relaunch the mod to apply.";
    else if (menu->status == SAVE_FAILED)
        hint = "Save failed. Check map IDs and file permissions.";
    draw_text(x, 531, hint, ctx);
    if (menu->editing == MAP_IDS + 1 && menu->status != EDIT_INVALID)
        draw_text(x, 558, "Separate map IDs with commas. Omit .map.", ctx);

    ui_text_position[0] = previous_x;
    ui_text_position[1] = previous_y;
    set_text_position(previous_x, previous_y);
    ui_text_color = previous_color;
    set_text_rgba(previous_color & 255, (previous_color >> 8) & 255, (previous_color >> 16) & 255, previous_color >> 24);
}
