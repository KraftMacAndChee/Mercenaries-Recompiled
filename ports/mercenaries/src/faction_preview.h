/* Read-only retail observations. Sampling is automatic in preview builds;
 * F8 bookmarks the existing history. HUD: two/second; named events: 32/second.
 * No guest calls, guest writes, allocation, heap walks or synchronous file I/O. */
static __declspec(thread) struct {
    uint32_t target, ai, vehicle, personal, samples;
    ULONGLONG last;
    int logged;
} preview_faction_query;

static int preview_faction_range(uint32_t address, uint32_t bytes)
{
    return address >= 0x10000u && bytes <= 0x4000000u &&
           address <= 0x4000000u - bytes;
}

static uint32_t preview_faction_guid(uint32_t actor)
{
    uint32_t metadata;
    if (!preview_faction_range(actor, 12u)) return 0;
    metadata = guest_u32(actor + 8u);
    return preview_faction_range(metadata, 0x2Cu) ? guest_u32(metadata + 0x28u) : 0;
}

void recomp_preview_faction_query(uint32_t phase, uint32_t target,
                                  uint32_t detail, uint32_t value)
{
    uint32_t owner, color, reason, ai_faction = 0xFFFFFFFFu;
    uint32_t enemies[4] = {0}, vehicle_guid, trespass = 0, visible_faction = 0;
    float times[4] = {0}, hostile_range = 0, standing = 0;
    ULONGLONG now;
    if (!xbox_preview_log_enabled()) return;
    if (phase == 0u) {
        preview_faction_query.target = target;
        preview_faction_query.ai = preview_faction_query.vehicle = 0;
        preview_faction_query.personal = 0xFFFFFFFFu;
        return;
    }
    if (!target || target != preview_faction_query.target) return;
    if (phase == 1u) {
        preview_faction_query.ai = detail;
        preview_faction_query.vehicle = value;
        return;
    }
    if (phase == 2u) {
        preview_faction_query.personal = value;
        return;
    }
    if (phase != 3u) return;
    ++preview_faction_query.samples;
    now = GetTickCount64();
    if (preview_faction_query.logged && now-preview_faction_query.last < 500u) return;
    if (!preview_faction_range(target, 0x60u) || detail >= 8u || value >= 4u) return;
    owner = guest_u32(target + 0x5Cu);
    reason = (preview_faction_query.personal < 2u ? 1u : 0u) |
             (value == 0u ? 2u : 0u);
    color = reason ? guest_u32(0x30E5A4u) :
            detail ? 0x80C02020u : guest_u32(0x30E5A0u);
    standing = guest_f32(0x323358u + detail * 32u);
    if (preview_faction_range(preview_faction_query.ai, 0x51Cu)) {
        const uint32_t ai = preview_faction_query.ai;
        const float base_time = guest_f32(ai + 0x434u);
        ai_faction = guest_u32(ai + 0x4A4u);
        hostile_range = guest_f32(ai + 0x518u);
        for (unsigned i = 0; i < 4; ++i) {
            enemies[i] = guest_u32(ai + 0x438u + 8u*i);
            if (enemies[i]) times[i] = base_time + guest_f32(ai + 0x43Cu + 8u*i);
        }
    }
    vehicle_guid = preview_faction_guid(preview_faction_query.vehicle);
    if (preview_faction_range(preview_faction_query.vehicle, 0x258u)) {
        trespass = guest_u32(preview_faction_query.vehicle + 0x254u);
        visible_faction = guest_u32(preview_faction_query.vehicle + 0xF4u);
    }
    xbox_preview_log_event("faction-target",
        "target=%08X owner=%08X guid=%08X ai=%08X ai_faction=%u "
        "player_vehicle=%08X player_guid=%08X visible_faction=%u trespass=%08X "
        "faction=%u standing=%.6g personal=%u global=%u red_reason=%u selected_color=%08X "
        "hostile_range=%.5g enemies=%08X:%.4g,%08X:%.4g,%08X:%.4g,%08X:%.4g samples=%u",
        target, owner, preview_faction_guid(owner), preview_faction_query.ai, ai_faction,
        preview_faction_query.vehicle, vehicle_guid, visible_faction, trespass,
        detail, standing, preview_faction_query.personal, value, reason, color,
        hostile_range, enemies[0], times[0], enemies[1], times[1],
        enemies[2], times[2], enemies[3], times[3], preview_faction_query.samples);
    preview_faction_query.samples = 0;
    preview_faction_query.last = now;
    preview_faction_query.logged = 1;
}

/* Only named HQ/faction callbacks; unrelated event updates are not logged. */
void recomp_preview_faction_event(uint32_t phase, uint32_t event)
{
    static __declspec(thread) ULONGLONG window;
    static __declspec(thread) uint32_t count, dropped;
    char name[96];
    uint32_t prefix;
    ULONGLONG now;
    if (!xbox_preview_log_enabled() || phase < 1u || phase > 3u ||
        !preview_faction_range(event, 0xACu)) return;
    prefix = guest_u32(event + 0x38u);
    if (prefix != 0x6E756F42u && prefix != 0x74636146u &&
        prefix != 0x79616C50u && prefix != 0x64697341u &&
        prefix != 0x73416F4Eu && prefix != 0x62616E45u) return;
    for (unsigned i = 0; i < sizeof(name); ++i) {
        name[i] = (char)(guest_u32(event + 0x38u + i) & 255u);
        if (!name[i]) break;
        if (i == sizeof(name)-1u) return;
    }
    if (strcmp(name, "BouncerUsed") && strcmp(name, "BouncerBribeRequested") &&
        strcmp(name, "FactionAttitudeChange") && strcmp(name, "PlayBriefingScript") &&
        strcmp(name, "AsideFadeIn") && strcmp(name, "NoAsideFadeIn") &&
        strcmp(name, "EnableExit")) return;
    now = GetTickCount64();
    if (now - window >= 1000u) { window = now; count = 0; }
    if (count >= 32u) { if (dropped != 0xFFFFFFFFu) ++dropped; return; }
    ++count;
    xbox_preview_log_sample("faction-event",
        "phase=%s event=%08X id=%u type=%u lua=%08X callback=%s "
        "data=%08X,%08X,%08X,%08X active=%u free=%u high_water=%u "
        "to_player=%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g dropped=%u",
        phase == 1u ? "dispatch-setup" : phase == 2u ? "cancel" : "registered",
        event, guest_u32(event+0xA8u), guest_u32(event+0x14u), guest_u32(event+4u), name,
        guest_u32(event+0x18u), guest_u32(event+0x1Cu),
        guest_u32(event+0x20u), guest_u32(event+0x24u),
        guest_u32(0x30E7A0u), guest_u32(0x30E7B0u), guest_u32(0x365EB4u),
        guest_f32(0x323358u), guest_f32(0x323378u), guest_f32(0x323398u), guest_f32(0x3233B8u),
        guest_f32(0x3233D8u), guest_f32(0x3233F8u), guest_f32(0x323418u), guest_f32(0x323438u), dropped);
    dropped = 0;
}
