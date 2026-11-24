/* Exchange the host's militia profile through the game's co-op Rules message. */
typedef unsigned char Byte;
typedef unsigned int U32;
typedef unsigned long long Size;

typedef struct {
    const Byte *data;
    Size length;
} Slice;

enum {
    RULES_MESSAGE = 0x11,
    MESSAGE_HEADER = 5,
    MAX_STOCK_PAYLOAD = 1 << 20,
    PROFILE_CAPACITY = 4096,
    TRAILER_SIZE = 6,
    MAP_LIST_OFFSET = 25,
};

extern const Byte militia_coop_enabled;
extern const Byte militia_local_rules[];
extern const U32 militia_local_rules_size;
extern Byte militia_guest_rules[PROFILE_CAPACITY];
extern Size militia_coop_ready_seed;
extern void send_coop_packet(const Slice *packet, void *context);
extern void parse_coop_rules(void *destination, const Slice *rules, void *context);
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *heap, U32 flags, Size bytes);
__declspec(dllimport) int HeapFree(void *heap, U32 flags, void *memory);

static Size profile_seed(const Byte *data, Size length)
{
    Size hash = 14695981039346656037ULL;
    for (Size i = 0; i < length; i++)
        hash = (hash ^ data[i]) * 1099511628211ULL;
    return hash ? hash : 1;
}

static int valid_profile(const Byte *data, Size length)
{
    if (length <= MAP_LIST_OFFSET || length > PROFILE_CAPACITY || data[24] > 1)
        return 0;
    for (Size offset = 0; offset < 24; offset += 8) {
        Size threshold = 0;
        for (Size i = 0; i < 8; i++)
            threshold |= (Size)data[offset + i] << (8 * i);
        if (threshold > (1ULL << 32))
            return 0;
    }
    Size offset = MAP_LIST_OFFSET;
    while (offset < length) {
        Size map_length = data[offset++];
        if (!map_length)
            return offset == length;
        if (map_length > 80 || map_length > length - offset)
            return 0;
        offset += map_length;
    }
    return 0;
}

void send_militia_rules(const Slice *packet, void *context)
{
    if (!militia_coop_enabled) {
        send_coop_packet(packet, context);
        return;
    }
    militia_coop_ready_seed = 0;
    Size profile_size = militia_local_rules_size;
    Size extra = profile_size + TRAILER_SIZE;
    if (packet->length < MESSAGE_HEADER || packet->data[0] != RULES_MESSAGE ||
        profile_size > PROFILE_CAPACITY || packet->length > MESSAGE_HEADER + MAX_STOCK_PAYLOAD - extra) {
        send_coop_packet(packet, context);
        return;
    }

    Size total = packet->length + extra;
    void *heap = GetProcessHeap();
    Byte *data = HeapAlloc(heap, 0, total);
    if (!data) {
        send_coop_packet(packet, context);
        return;
    }
    for (Size i = 0; i < packet->length; i++)
        data[i] = packet->data[i];
    for (Size i = 0; i < profile_size; i++)
        data[packet->length + i] = militia_local_rules[i];

    Size payload = total - MESSAGE_HEADER;
    for (Size i = 0; i < 4; i++)
        data[1 + i] = (Byte)(payload >> (8 * i));
    Byte *trailer = data + total - TRAILER_SIZE;
    trailer[0] = 'A';
    trailer[1] = 'C';
    trailer[2] = 'M';
    trailer[3] = 1;
    trailer[4] = (Byte)profile_size;
    trailer[5] = (Byte)(profile_size >> 8);

    Slice extended = {data, total};
    militia_coop_ready_seed = profile_seed(militia_local_rules, profile_size);
    send_coop_packet(&extended, context);
    HeapFree(heap, 0, data);
}

void receive_militia_rules(void *destination, const Slice *rules, void *context)
{
    Slice stock = *rules;
    if (militia_coop_enabled) {
        militia_coop_ready_seed = 0;
        if (rules->length >= TRAILER_SIZE) {
            const Byte *trailer = rules->data + rules->length - TRAILER_SIZE;
            if (trailer[0] == 'A' && trailer[1] == 'C' && trailer[2] == 'M' && trailer[3] == 1) {
                Size size = trailer[4] | ((Size)trailer[5] << 8);
                if (size <= rules->length - TRAILER_SIZE) {
                    stock.length -= size + TRAILER_SIZE;
                    const Byte *profile = stock.data + stock.length;
                    if (valid_profile(profile, size)) {
                        for (Size i = 0; i < size; i++)
                            militia_guest_rules[i] = profile[i];
                        militia_coop_ready_seed = profile_seed(profile, size);
                    }
                }
            }
        }
    }
    parse_coop_rules(destination, &stock, context);
}
