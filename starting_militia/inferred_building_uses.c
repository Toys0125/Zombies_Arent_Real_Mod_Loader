// Read-only view of the game's footprint data for the currently loaded map.
typedef unsigned char Byte;
typedef unsigned int U32;
typedef unsigned long long Size;

typedef struct {
    float x, y;
} Point;

typedef struct {
    unsigned short height;
    Byte roof, use;
} BuildingAttribute;

typedef struct {
    int offset, count;
} BuildingPlaceRange;

typedef struct {
    int category, count;
} BuildingPlaceType;

typedef struct {
    const char *text;
    Size length;
} PlaceCategory;

extern const Point *building_lower_bounds, *building_upper_bounds;
extern const Point *building_vertices;
extern const int *building_vertex_offsets, *building_vertex_lengths;
extern const U32 *building_grid_offsets;
extern const int *building_grid_indices;
extern const BuildingAttribute *building_attributes;
extern Size building_count, building_attribute_count;
extern Size building_lower_bound_count, building_upper_bound_count;
extern Size building_vertex_count, building_vertex_offset_count, building_vertex_length_count;
extern Size building_grid_width, building_grid_height, building_grid_offset_count, building_grid_index_count;

extern const BuildingPlaceRange *building_place_ranges;
extern const BuildingPlaceType *building_place_types;
extern const PlaceCategory *place_categories;
extern Size building_place_range_count, building_place_type_count, place_category_count;

extern const char *active_map_path;
extern Size active_map_path_length;
extern Point active_map_origin;
typedef struct {
    U32 count, size;
    char name[80];
} GisMap;

extern const U32 gis_map_count;
extern const GisMap gis_maps[];

extern Byte *inferred_building_uses;
extern Size inferred_building_count;
extern const GisMap *inferred_gis_map;

__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *heap, U32 flags, Size bytes);
__declspec(dllimport) int HeapFree(void *heap, U32 flags, void *memory);

typedef struct {
    Point center, axis;
    float area, width;
    Byte use, house_shape, valid, gap_shape;
} Footprint;

typedef struct {
    float distance_sq;
    int index;
} Neighbor;

static float minimum(float a, float b) { return a < b ? a : b; }
static float maximum(float a, float b) { return a > b ? a : b; }
static float absolute(float a) { return a < 0 ? -a : a; }

// Low two bits are the inferred use. The GIS bit lets the hover card name the source.
enum { RESIDENTIAL = 1, COMMERCIAL = 2, GIS_SOURCE = 4, PENDING_HOME = 8 };

static Byte native_use(Size index)
{
    return building_attributes && index < building_attribute_count ? building_attributes[index].use : 0;
}

static int category_branch(PlaceCategory category, const char *branch, Size length)
{
    if (!category.text || category.length < length)
        return 0;
    for (Size i = 0; i < length; i++)
        if (category.text[i] != branch[i])
            return 0;
    return category.length == length || category.text[length] == '/';
}

static int is_storefront(PlaceCategory category)
{
    if (category_branch(category, "shopping", sizeof("shopping") - 1)
        || category_branch(category, "food_and_drink", sizeof("food_and_drink") - 1))
        return 1;

    // Personal service listings also include home-based work. Match the
    // specific salon and barbershop branches instead of the whole category.
    static const char personal_services[] = "lifestyle_services/personal_or_beauty_service";
    const Size prefix_length = sizeof(personal_services);
    if (category.length <= prefix_length || !category_branch(category, personal_services, prefix_length - 1))
        return 0;
    const PlaceCategory service = {category.text + prefix_length, category.length - prefix_length};
    return category_branch(service, "hair_salon", sizeof("hair_salon") - 1)
        || category_branch(service, "barber", sizeof("barber") - 1)
        || category_branch(service, "beauty_salon", sizeof("beauty_salon") - 1)
        || category_branch(service, "nail_salon", sizeof("nail_salon") - 1)
        || category_branch(service, "tanning_salon", sizeof("tanning_salon") - 1);
}

static void mark_commercial_uses(Byte *uses, Size building_total)
{
    if (!building_place_ranges || !building_place_types || !place_categories)
        return;
    const Size limit = building_total < building_place_range_count ? building_total : building_place_range_count;
    for (Size i = 0; i < limit; i++) {
        const BuildingPlaceRange range = building_place_ranges[i];
        if (range.count <= 0 || range.offset < 0
            || (Size)range.offset + (Size)range.count > building_place_type_count || native_use(i))
            continue;
        for (int j = 0; j < range.count; j++) {
            const BuildingPlaceType type = building_place_types[(Size)range.offset + j];
            if (type.count <= 0 || type.category < 0 || (Size)type.category >= place_category_count)
                continue;
            if (is_storefront(place_categories[type.category])) {
                uses[i] = COMMERCIAL;
                break;
            }
        }
    }
}

static const GisMap *active_gis_map(void)
{
    if (!gis_map_count || !active_map_path)
        return 0;
    Size start = active_map_path_length;
    while (start && active_map_path[start - 1] != '/' && active_map_path[start - 1] != '\\')
        start--;
    const Size length = active_map_path_length - start;
    if (!length || length >= sizeof(gis_maps[0].name))
        return 0;

    const GisMap *map = gis_maps;
    for (U32 i = 0; i < gis_map_count; i++) {
        Size j = 0;
        while (j < length) {
            char c = active_map_path[start + j];
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if (c != map->name[j])
                break;
            j++;
        }
        if (j == length && !map->name[j])
            return map;
        map = (const GisMap *)((const Byte *)(map + 1) + ((map->size + 3) & ~3U));
    }
    return 0;
}

static int decode_gis_keys(const GisMap *map, Size *entries)
{
    U32 offset = 0;
    Size previous = 0;
    for (U32 i = 0; i < map->count; i++) {
        Size delta = 0;
        Byte byte;
        unsigned int shift = 0;
        do {
            if (offset >= map->size || shift >= 35)
                return 0;
            byte = ((const Byte *)(map + 1))[offset++];
            delta |= (Size)(byte & 127) << shift;
            shift += 7;
        } while (byte & 128);
        if ((i && !delta) || delta > 0x1ffffffffULL - previous)
            return 0;
        previous += delta;
        entries[i] = previous;
    }
    return offset == map->size;
}

static Byte gis_building_use(const Size *entries, U32 entry_count, Size index)
{
    const Point lower = building_lower_bounds[index], upper = building_upper_bounds[index];
    // Recover the uncropped center, then find it in the half-meter lookup.
    const double x = (double)lower.x + upper.x + 2.0 * active_map_origin.x;
    const double y = (double)lower.y + upper.y + 2.0 * active_map_origin.y;
    if (!(x >= -0.49 && y >= -0.49 && x <= 65535.49 && y <= 65535.49))
        return 0;
    const U32 qx = (U32)(x + 0.5), qy = (U32)(y + 0.5);
    if (x - qx <= -0.49 || x - qx >= 0.49 || y - qy <= -0.49 || y - qy >= 0.49)
        return 0;
    const Size key = ((Size)qx << 16) | qy;
    U32 lo = 0, hi = entry_count;
    while (lo < hi) {
        const U32 mid = lo + (hi - lo) / 2;
        if ((entries[mid] >> 1) < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < entry_count && (entries[lo] >> 1) == key)
        return (Byte)(((entries[lo] & 1) + 1) | GIS_SOURCE);
    return 0;
}

static void mark_gis_uses(const GisMap *map, void *heap, Byte *uses, Size building_total)
{
    Size *entries = HeapAlloc(heap, 0, (Size)map->count * sizeof(*entries));
    if (!entries)
        return;
    if (decode_gis_keys(map, entries)) {
        for (Size i = 0; i < building_total; i++)
            if (!uses[i] && !native_use(i))
                uses[i] = gis_building_use(entries, map->count, i);
    }
    HeapFree(heap, 0, entries);
}

static void read_footprint(Footprint *footprint, Size index)
{
    const int offset = building_vertex_offsets[index];
    const int length = building_vertex_lengths[index];
    if (offset < 0 || length < 3 || (Size)offset + (Size)length > building_vertex_count)
        return;

    const Point *vertices = building_vertices + offset;
    const Point lower = building_lower_bounds[index], upper = building_upper_bounds[index];
    const Point center = {(lower.x + upper.x) * 0.5f, (lower.y + upper.y) * 0.5f};
    footprint->center = center;
    footprint->use = native_use(index);

    Point longest = {0, 0};
    Point previous = vertices[length - 1];
    float edge_sq = 0, twice_area = 0;
    for (int i = 0; i < length; i++) {
        const Point p = vertices[i];
        const Point edge = {p.x - previous.x, p.y - previous.y};
        const float length_sq = edge.x * edge.x + edge.y * edge.y;
        if (length_sq > edge_sq) {
            edge_sq = length_sq;
            longest = edge;
        }
        twice_area += (previous.x - center.x) * (p.y - center.y)
                    - (previous.y - center.y) * (p.x - center.x);
        previous = p;
    }

    const float area = absolute(twice_area) * 0.5f;
    if (!(area > 1))
        return;
    footprint->valid = 1;
    footprint->area = area;
    if (!(area >= 50 && area <= 425 && edge_sq > 0))
        return;

    const float length_inverse = 1.0f / __builtin_sqrtf(edge_sq);
    const Point axis = {longest.x * length_inverse, longest.y * length_inverse};
    const float first_dx = vertices[0].x - center.x, first_dy = vertices[0].y - center.y;
    float x_min = first_dx * axis.x + first_dy * axis.y, x_max = x_min;
    float y_min = first_dy * axis.x - first_dx * axis.y, y_max = y_min;
    for (int i = 1; i < length; i++) {
        const float dx = vertices[i].x - center.x, dy = vertices[i].y - center.y;
        const float x = dx * axis.x + dy * axis.y, y = dy * axis.x - dx * axis.y;
        x_min = minimum(x_min, x);
        x_max = maximum(x_max, x);
        y_min = minimum(y_min, y);
        y_max = maximum(y_max, y);
    }
    const float width = minimum(x_max - x_min, y_max - y_min);
    const float length_along_axis = maximum(x_max - x_min, y_max - y_min);
    footprint->axis = axis;
    footprint->width = width;
    footprint->gap_shape = width >= 4.5f && width <= 20 && length_along_axis >= 7.5f
        && length_along_axis <= 45 && area >= 0.48f * width * length_along_axis;
    if (width >= 4.5f && width <= 17 && length_along_axis >= 7.5f && length_along_axis <= 43
        && area >= 0.60f * width * length_along_axis)
        footprint->house_shape = 1;
}

static void keep_nearest(Neighbor *neighbors, int count, int index, float distance_sq)
{
    int i = count - 1;
    const Neighbor last = neighbors[i];
    if (distance_sq > last.distance_sq
        || (distance_sq == last.distance_sq && last.index >= 0 && last.index <= index))
        return;
    while (i > 0) {
        const Neighbor prior = neighbors[i - 1];
        if (prior.distance_sq < distance_sq || (prior.distance_sq == distance_sq && prior.index < index))
            break;
        i--;
    }
    if (neighbors[i].index == index)
        return;
    while (count - 1 > i) {
        neighbors[count - 1] = neighbors[count - 2];
        count--;
    }
    neighbors[i] = (Neighbor){distance_sq, index};
}

enum { NEARBY_COUNT = 16, DENSITY_COUNT = 12, TAGGED_COUNT = 8, RESIDENTIAL_CONTEXT_RADIUS = 40 };

static int fills_residential_gap(const Footprint *footprints, const Footprint *candidate,
                                 const Neighbor *nearby, int use_inferred_homes)
{
    Point directions[NEARBY_COUNT];
    float distances[NEARBY_COUNT];
    const float radius = use_inferred_homes ? RESIDENTIAL_CONTEXT_RADIUS : 35;
    int anchors = 0, close_homes = 0, adjacent_home = 0, confirmed_homes = 0;
    for (int i = 0; i < NEARBY_COUNT; i++) {
        const int index = nearby[i].index;
        const float distance_sq = nearby[i].distance_sq;
        if (index < 0 || distance_sq > radius * radius)
            break;
        const Footprint *peer = &footprints[index];
        const Byte inferred = inferred_building_uses[index];
        Byte use = peer->use;
        if (!use && (inferred == COMMERCIAL || (inferred & GIS_SOURCE)
                     || (use_inferred_homes && inferred == RESIDENTIAL)))
            use = inferred & (GIS_SOURCE - 1);
        if (use && use != RESIDENTIAL && use != 13 && distance_sq <= 15 * 15)
            return 0;
        if (use != RESIDENTIAL || !peer->gap_shape || distance_sq < 5 * 5
            || peer->area < candidate->area * 0.45f || peer->area > candidate->area * 2.2f
            || peer->width < candidate->width * 0.55f || peer->width > candidate->width * 1.8f)
            continue;
        directions[anchors] = (Point){peer->center.x - candidate->center.x,
                                      peer->center.y - candidate->center.y};
        distances[anchors++] = distance_sq;
        close_homes += distance_sq <= 24 * 24;
        adjacent_home |= distance_sq <= 12 * 12;
        confirmed_homes += peer->use == RESIDENTIAL || inferred == (RESIDENTIAL | GIS_SOURCE);
    }
    // Require a confirmed home on each side. A 120-degree minimum angle
    // accepts bends and variable setbacks without extending a row past its end.
    for (int i = 0; i < anchors; i++) {
        for (int j = i + 1; j < anchors; j++) {
            const float dot = directions[i].x * directions[j].x + directions[i].y * directions[j].y;
            if (dot < 0 && dot * dot >= 0.25f * distances[i] * distances[j])
                return 1;
        }
    }
    // A tight run of matching homes can end on a curve or have several
    // missing tags in a row. Require direct evidence for the wider cluster.
    return use_inferred_homes && ((adjacent_home && close_homes >= 3)
                                  || (confirmed_homes >= 2 && anchors >= 5));
}

static int grid_cell(float coordinate, int dimension)
{
    if (coordinate <= 0)
        return 0;
    const float cell = coordinate / 64;
    return cell < dimension ? (int)cell : dimension - 1;
}

static int looks_residential(const Footprint *footprints, Size count, int index, int use_inferred_homes)
{
    const Footprint *home = &footprints[index];
    Neighbor nearby[NEARBY_COUNT], tagged[TAGGED_COUNT];
    for (int i = 0; i < NEARBY_COUNT; i++)
        nearby[i] = (Neighbor){64 * 64, -1};
    for (int i = 0; i < TAGGED_COUNT; i++)
        tagged[i] = (Neighbor){120 * 120, -1};

    const int width = (int)building_grid_width, height = (int)building_grid_height;
    const float radius = use_inferred_homes ? RESIDENTIAL_CONTEXT_RADIUS : 120;
    const int min_x = grid_cell(home->center.x - radius, width);
    const int max_x = grid_cell(home->center.x + radius, width);
    const int min_y = grid_cell(home->center.y - radius, height);
    const int max_y = grid_cell(home->center.y + radius, height);
    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            const Size cell = (Size)y * width + x;
            const U32 start = building_grid_offsets[cell], end = building_grid_offsets[cell + 1];
            if (end > building_grid_index_count)
                continue;
            for (U32 i = start; i < end; i++) {
                const int other = building_grid_indices[i];
                if (other == index || other < 0 || (Size)other >= count || !footprints[other].valid)
                    continue;
                const Point p = footprints[other].center;
                const float dx = p.x - home->center.x, dy = p.y - home->center.y;
                const float distance_sq = dx * dx + dy * dy;
                keep_nearest(nearby, NEARBY_COUNT, other, distance_sq);
                if (!use_inferred_homes && footprints[other].use)
                    keep_nearest(tagged, TAGGED_COUNT, other, distance_sq);
            }
        }
    }

    if (fills_residential_gap(footprints, home, nearby, use_inferred_homes))
        return 1;
    if (use_inferred_homes || !home->house_shape)
        return 0;

    int cluster = 0;
    for (int i = 0; i < DENSITY_COUNT; i++)
        if (nearby[i].index >= 0)
            cluster += footprints[nearby[i].index].house_shape;
    if (cluster < 7)
        return 0;

    unsigned int directions = 0;
    int row_x = 0, row_y = 0, curved_neighbors = 0;
    for (int i = 0; i < NEARBY_COUNT; i++) {
        if (nearby[i].index < 0 || nearby[i].distance_sq > 28 * 28)
            break;
        const Footprint *peer = &footprints[nearby[i].index];
        if (nearby[i].distance_sq < 3 * 3 || !peer->house_shape
            || peer->area < home->area * 0.55f || peer->area > home->area * 1.82f
            || peer->width < home->width * 0.65f || peer->width > home->width * 1.54f)
            continue;

        // The longest edges may point along either axis of a rectangular footprint.
        const float dot = home->axis.x * peer->axis.x + home->axis.y * peer->axis.y;
        const float cross = home->axis.x * peer->axis.y - home->axis.y * peer->axis.x;
        const float alignment = maximum(absolute(dot), absolute(cross));
        if (alignment < 0.913545f) // cos(24 degrees)
            continue;
        const float dx = peer->center.x - home->center.x, dy = peer->center.y - home->center.y;
        const float x = dx * home->axis.x + dy * home->axis.y;
        const float y = dy * home->axis.x - dx * home->axis.y;
        const float abs_x = absolute(x), abs_y = absolute(y);
        const float lateral = minimum(abs_x, abs_y);
        if (lateral <= 7 && lateral <= 0.45f * maximum(abs_x, abs_y))
            curved_neighbors++;

        if (alignment < 0.970296f) // cos(14 degrees)
            continue;
        if (abs_y <= 4) {
            row_x++;
            directions |= x > 0 ? 1 : 2;
        }
        if (abs_x <= 4) {
            row_y++;
            directions |= y > 0 ? 4 : 8;
        }
    }
    const int inside_row = (directions & 3) == 3 || (directions & 12) == 12;
    // At row ends or bends, require a denser cluster to compensate for
    // neighbors that are one-sided, slightly rotated, or set back.
    const int row_end = cluster >= 8 && (row_x >= 2 || row_y >= 2);
    const int street_bend = cluster >= 9 && curved_neighbors >= 3 && (row_x || row_y);
    if (!inside_row && !row_end && !street_bend)
        return 0;

    float residential = 0, other_use = 0;
    for (int i = 0; i < TAGGED_COUNT && tagged[i].index >= 0; i++) {
        const Byte use = footprints[tagged[i].index].use;
        const float influence = 1 - tagged[i].distance_sq / (120 * 120);
        const float weight = influence * influence * influence;
        if (use == RESIDENTIAL)
            residential += weight;
        else if (use != 13) // A tagged shed is compatible with a residential block.
            other_use += weight;
    }
    const float max_other_use = inside_row ? 1.1f : 0.8f;
    const float residential_ratio = inside_row ? 1.0f : 1.5f;
    return other_use <= max_other_use || residential >= residential_ratio * other_use;
}

void refresh_inferred_building_uses(void)
{
    const Size count = building_count;
    const GisMap *gis_map = active_gis_map();
    // The spatial-index rebuild invalidates this count before installing a map.
    if (!count || (inferred_building_count == count && inferred_gis_map == gis_map))
        return;
    if (count > 0x7fffffff || building_vertex_offset_count < count || building_vertex_length_count < count
        || building_lower_bound_count < count || building_upper_bound_count < count
        || !building_vertices || !building_vertex_offsets || !building_vertex_lengths
        || !building_lower_bounds || !building_upper_bounds || !building_grid_offsets || !building_grid_indices
        || !building_grid_width || !building_grid_height
        || building_grid_width > 0x7fffffff || building_grid_height > 0x7fffffff
        || building_grid_width * building_grid_height >= building_grid_offset_count)
        return;

    void *heap = GetProcessHeap();
    if (inferred_building_uses)
        HeapFree(heap, 0, inferred_building_uses);
    inferred_building_count = count;
    inferred_gis_map = gis_map;
    inferred_building_uses = HeapAlloc(heap, 8, count); // HEAP_ZERO_MEMORY
    if (!inferred_building_uses)
        return;
    mark_commercial_uses(inferred_building_uses, count);
    if (gis_map)
        mark_gis_uses(gis_map, heap, inferred_building_uses, count);

    Footprint *footprints = HeapAlloc(heap, 8, count * sizeof(*footprints)); // HEAP_ZERO_MEMORY
    if (!footprints)
        return;
    for (Size i = 0; i < count; i++)
        read_footprint(&footprints[i], i);
    for (Size i = 0; i < count; i++)
        if (!inferred_building_uses[i] && !footprints[i].use && footprints[i].gap_shape
            && looks_residential(footprints, count, (int)i, 0))
            inferred_building_uses[i] = RESIDENTIAL;
    // Freeze the first pass's homes as evidence. The pending values cannot
    // act as anchors, so this pass gives the same answer in any scan order.
    for (Size i = 0; i < count; i++)
        if (!inferred_building_uses[i] && !footprints[i].use && footprints[i].gap_shape
            && looks_residential(footprints, count, (int)i, 1))
            inferred_building_uses[i] = PENDING_HOME;
    for (Size i = 0; i < count; i++)
        if (inferred_building_uses[i] == PENDING_HOME)
            inferred_building_uses[i] = RESIDENTIAL;
    HeapFree(heap, 0, footprints);
}
