/* Bounded crash evidence for the event-list failure in dump 2052. This only
 * observes callbacks; it does not change guest registers, lists or dispatch. */
typedef struct RecompEventHistory {
    uint32_t node, returned_node, next, object, state, type, id, stack;
    char callback[25];
} RecompEventHistory;
static RecompEventHistory g_event_history[32];
static uint32_t g_event_history_cursor, g_event_history_available;
static int g_event_history_reported;

static int recomp_event_readable(uint32_t address, uint32_t bytes)
{
    return address >= 0x10000u && address <= 0x04000000u - bytes;
}

static void recomp_event_record_return(uint32_t node, uint32_t expected_esp,
                                      uint32_t saved_edi)
{
    RecompEventHistory sample = {0};
    int bad = g_esi != node || g_esp != expected_esp || g_edi != saved_edi;
    sample.node = node;
    sample.returned_node = g_esi;
    sample.stack = g_esp;
    if (recomp_event_readable(node, 12u)) {
        sample.next = guest_u32(node);
        sample.object = guest_u32(node + 8u);
    } else {
        bad = 1;
    }
    if (!recomp_event_readable(sample.next, 12u))
        bad = 1;
    if (recomp_event_readable(sample.object, 0xACu)) {
        sample.state = guest_u32(sample.object + 4u);
        sample.type = guest_u32(sample.object + 0x14u);
        sample.id = guest_u32(sample.object + 0xA8u);
        for (uint32_t i = 0; i < 24u; ++i)
            sample.callback[i] = (char)(guest_u32(sample.object + 0x38u + (i & ~3u)) >> ((i & 3u) * 8u));
    }
    g_event_history[g_event_history_cursor++ & 31u] = sample;
    if (g_event_history_available < 32u)
        ++g_event_history_available;
    if (!bad || g_event_history_reported)
        return;
    g_event_history_reported = 1;
    char context[12288];
    size_t used = 0;
    context[0] = 0;
    for (uint32_t i = 0; i < g_event_history_available; ++i) {
        const RecompEventHistory *h = &g_event_history[
            (g_event_history_cursor - g_event_history_available + i) & 31u];
        int n = snprintf(context + used, sizeof(context) - used,
            "[EVENT-HISTORY] node=%08X returned=%08X next=%08X object=%08X "
            "state=%08X type=%u id=%08X sp=%08X callback=%.24s\n",
            h->node,h->returned_node,h->next,h->object,h->state,
            h->type,h->id,h->stack,h->callback);
        if (n < 0 || (size_t)n >= sizeof(context) - used)
            break;
        used += (size_t)n;
    }
    xbox_preview_log_set_crash_context(context);
    xbox_preview_log_event("EVENT-LIST-INVALID",
        "node=%08X returned=%08X next=%08X object=%08X sp=%08X/%08X "
        "edi=%08X/%08X last=%08X history=%u",
        node,g_esi,sample.next,sample.object,g_esp,expected_esp,
        g_edi,saved_edi,g_recomp_current_func,g_event_history_available);
}
