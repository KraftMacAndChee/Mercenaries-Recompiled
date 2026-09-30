/* Read-only camera telemetry. Called at retail 9E183 / 9E1D5 / 9E2A3.
 * File I/O stays on the preview logger's worker; ordinary on-foot play does not produce these records. */
static struct {
    uint32_t state, vehicle, obstacle, count;
    ULONGLONG logged, active_until;
    float desired[3], hit[3], shortest, longest;
    int pending, have_hit;
} preview_winch_camera;

static void preview_winch_pre(uint32_t state, uint32_t position)
{
    float blend;
    ULONGLONG now;
    preview_winch_camera.pending = 0;
    if (!xbox_preview_log_enabled() || state < 0x10000u ||
        state > 0x4000000u - 0x184u || position < 0x10000u ||
        position > 0x4000000u - 12u || guest_u32(state) != 0x2E6FD4u)
        return;
    blend = guest_f32(state + 0x180u);
    if (!(blend > 0) && !preview_winch_camera.active_until) return;
    now = GetTickCount64();
    if (blend > 0) preview_winch_camera.active_until = now + 2000;
    else if (now > preview_winch_camera.active_until) {
        preview_winch_camera.active_until = 0;
        preview_winch_camera.count = 0;
        return;
    }
    if (preview_winch_camera.state != state) {
        preview_winch_camera.count = 0;
        preview_winch_camera.logged = 0;
    }
    preview_winch_camera.state = state;
    preview_winch_camera.vehicle = g_edi;
    preview_winch_camera.obstacle = g_eax;
    for (uint32_t i = 0; i < 3; ++i)
        preview_winch_camera.desired[i] = guest_f32(position + 4*i);
    preview_winch_camera.have_hit = 0;
    preview_winch_camera.pending = 1;
}

static void preview_winch_hit(uint32_t site, uint32_t state, uint32_t result)
{
    if (site != 0x9E1D5u || !preview_winch_camera.pending ||
        state != preview_winch_camera.state || result < 0x10000u ||
        result > 0x4000000u - 12u) return;
    for (uint32_t i = 0; i < 3; ++i)
        preview_winch_camera.hit[i] = guest_f32(result + 4*i);
    preview_winch_camera.have_hit = 1;
}

static void preview_winch_post(uint32_t site, uint32_t state,
                               uint32_t focus, uint32_t direction,
                               uint32_t length_bits)
{
    float length, wanted = 0, output[3];
    ULONGLONG now;
    if (site != 0x9E1D5u || !preview_winch_camera.pending ||
        !preview_winch_camera.have_hit || state != preview_winch_camera.state ||
        focus < 0x10000u || focus > 0x4000000u - 12u ||
        direction < 0x10000u || direction > 0x4000000u - 12u) return;
    preview_winch_camera.pending = 0;
    memcpy(&length, &length_bits, 4);
    if (!preview_winch_camera.count || length < preview_winch_camera.shortest)
        preview_winch_camera.shortest = length;
    if (!preview_winch_camera.count || length > preview_winch_camera.longest)
        preview_winch_camera.longest = length;
    ++preview_winch_camera.count;
    now = GetTickCount64();
    if (preview_winch_camera.logged && now-preview_winch_camera.logged < 1000) return;
    for (uint32_t i = 0; i < 3; ++i) {
        float f = guest_f32(focus + 4*i);
        float d = preview_winch_camera.desired[i] - f;
        wanted += d*d;
        output[i] = f + guest_f32(direction + 4*i)*guest_f32(state + 0xF8u);
    }
    xbox_preview_log_event("winch-camera",
        "state=%08X vehicle=%08X self_obstacle=%08X mode=%08X guid=%08X "
        "blend=%.4g samples=%u wanted=%.5g collision=%.5g range=%.5g..%.5g "
        "stick=%.5g min=%.5g desired=(%.5g,%.5g,%.5g) "
        "hit=(%.5g,%.5g,%.5g) output=(%.5g,%.5g,%.5g)",
        state, preview_winch_camera.vehicle, preview_winch_camera.obstacle,
        guest_u32(state+0x158u), guest_u32(state+0x60u), guest_f32(state+0x180u),
        preview_winch_camera.count, sqrtf(wanted), length,
        preview_winch_camera.shortest, preview_winch_camera.longest,
        guest_f32(state+0xF8u), guest_f32(state+0xFCu),
        preview_winch_camera.desired[0], preview_winch_camera.desired[1],
        preview_winch_camera.desired[2], preview_winch_camera.hit[0],
        preview_winch_camera.hit[1], preview_winch_camera.hit[2],
        output[0], output[1], output[2]);
    preview_winch_camera.logged = now;
    preview_winch_camera.count = 0;
}
