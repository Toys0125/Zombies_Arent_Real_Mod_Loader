typedef unsigned char Byte;
typedef unsigned long long Count;

typedef struct {
    Count chances[3];
    Byte all_maps;
    Byte map_ids[];
} MilitiaRules;

extern const MilitiaRules militia_local_rules, militia_guest_rules;
extern const Byte daily_challenge_active, sandbox_active, campaign_active;
extern const Byte free_play_save_selected, modded_save_active;
extern const Byte coop_active, coop_side, coop_starting_battle, militia_coop_enabled;
extern const Count militia_coop_ready_seed;
extern Byte count_militia_this_battle;
extern const Byte *active_map_path;
extern const Count active_map_path_length;

extern const Count *civilian_positions;
extern const Byte *unit_kinds, *building_attributes, *inferred_building_uses;
extern const Count unit_count, building_count, building_attribute_count, inferred_building_count;
extern Count find_building_near_position(Count position);
extern void refresh_inferred_building_uses(void);

// In the same order as the spawn thresholds: other, residential, commercial.
extern Count militia_qualified[3];
extern Byte militia_spawn_info_ready;
extern Byte militia_spawn_lines[4][48];
extern const Count *militia_preview_positions;
extern Count militia_preview_count, militia_preview_profile;

enum { SPAWN_RESULT = 1, SPAWN_ESTIMATE = 2 };

static int is_supported_map(const MilitiaRules *rules)
{
    const Byte *path = active_map_path;
    Count length = active_map_path_length;
    if (!path || length < 5 || path[length - 4] != '.' || path[length - 3] != 'm'
        || path[length - 2] != 'a' || path[length - 1] != 'p')
        return 0;
    length -= 4;
    Count start = length;
    while (start && path[start - 1] != '/' && path[start - 1] != '\\')
        start--;
    path += start;
    length -= start;

    const char prefix[] = "custom_us_";
    if (length > sizeof(prefix) - 1) {
        Count i = 0;
        while (i < sizeof(prefix) - 1 && path[i] == (Byte)prefix[i])
            i++;
        if (i == sizeof(prefix) - 1)
            return 1;
    }
    for (const Byte *entry = rules->map_ids; *entry; entry += *entry + 1) {
        if (*entry != length)
            continue;
        Count i = 0;
        while (i < length && path[i] == entry[i + 1])
            i++;
        if (i == length)
            return 1;
    }
    return 0;
}

const MilitiaRules *select_militia_rules(int planning)
{
    if (daily_challenge_active)
        return 0;

    const MilitiaRules *rules = &militia_local_rules;
    if (coop_active) {
        if (!militia_coop_enabled || (!planning && !coop_starting_battle)
            || ((!planning || coop_side == 1) && !militia_coop_ready_seed))
            return 0;
        if (coop_side == 1)
            rules = &militia_guest_rules;
    } else if (!sandbox_active && !(free_play_save_selected ? modded_save_active : campaign_active)) {
        return 0;
    }

    if (!planning)
        count_militia_this_battle = 1;
    if (!(rules->chances[0] | rules->chances[1] | rules->chances[2]))
        return 0;
    return rules->all_maps || is_supported_map(rules) ? rules : 0;
}

unsigned classify_starting_civilian(Count position)
{
    const Count building = find_building_near_position(position);
    if (building < building_attribute_count && building_attributes) {
        const Byte use = building_attributes[building * 4 + 3];
        if (use)
            return use <= 2 ? use : 0;
    }
    if (building < inferred_building_count && inferred_building_uses) {
        const Byte use = inferred_building_uses[building] & 3;
        return use <= 2 ? use : 0;
    }
    return 0;
}

static void count_line(Byte *line, const char *label, Count count)
{
    Byte *text = line + 1;
    while (*label)
        *text++ = (Byte)*label++;

    Byte digits[26];
    int length = 0, group = 0;
    do {
        if (group == 3) {
            digits[length++] = ',';
            group = 0;
        }
        digits[length++] = (Byte)('0' + count % 10);
        count /= 10;
        group++;
    } while (count);
    while (length)
        *text++ = digits[--length];
    line[0] = (Byte)(text - line - 1);
}

static void show_counts(const Count eligible[3], const char *total_label, Count total, Byte status)
{
    count_line(militia_spawn_lines[0], "Residential ", eligible[1]);
    count_line(militia_spawn_lines[1], "Commercial ", eligible[2]);
    count_line(militia_spawn_lines[2], "Elsewhere ", eligible[0]);
    count_line(militia_spawn_lines[3], total_label, total);
    militia_spawn_info_ready = status;
}

void format_spawn_info(Count spawned)
{
    show_counts(militia_qualified, "Militia spawned ", spawned, SPAWN_RESULT);
}

void update_spawn_estimate(void)
{
    const MilitiaRules *rules = select_militia_rules(1);
    if (!rules || !unit_count || !civilian_positions || !unit_kinds) {
        militia_spawn_info_ready = 0;
        return;
    }
    const Count profile = rules == &militia_guest_rules ? militia_coop_ready_seed : 0;
    if (militia_spawn_info_ready == SPAWN_ESTIMATE && militia_preview_count == unit_count
        && militia_preview_positions == civilian_positions && militia_preview_profile == profile)
        return;

    refresh_inferred_building_uses();
    if (building_count && inferred_building_count != building_count) {
        militia_spawn_info_ready = 0;
        return;
    }
    Count eligible[3] = {0, 0, 0};
    for (Count i = 0; i < unit_count; i++) {
        if (unit_kinds[i] == 0) // The same civilian kind filtered by the stock arming loop.
            eligible[building_count ? classify_starting_civilian(civilian_positions[i]) : 0]++;
    }
    // Accumulate expected value before rounding so small categories still contribute.
    Count whole = 0, fraction = 1ULL << 31;
    for (unsigned i = 0; i < 3; i++) {
        const Count scaled = (eligible[i] & 0xffffffff) * rules->chances[i];
        whole += (eligible[i] >> 32) * rules->chances[i] + (scaled >> 32);
        fraction += scaled & 0xffffffff;
    }
    militia_preview_count = unit_count;
    militia_preview_positions = civilian_positions;
    militia_preview_profile = profile;
    show_counts(eligible, "Expected militia ", whole + (fraction >> 32), SPAWN_ESTIMATE);
}
