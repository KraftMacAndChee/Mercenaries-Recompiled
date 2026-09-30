/* Included after dev_spawn.h. Guest actors own rendering, animation and teardown;
 * the native flock core owns only steering. Called on the serialized world thread. */
#include "boids.h"
#include "boid_wings.h"
#include "dev_player_actor.h"

static boid_flock boid_world;
static uint32_t boid_guids[RECOMP_BOID_COUNT];
static uint32_t boid_names[RECOMP_BOID_COUNT];
static boid_wings boid_wing_states[RECOMP_BOID_COUNT];
static uint32_t boid_tracks[RECOMP_BOID_COUNT];
static float boid_track_rates[RECOMP_BOID_COUNT];
static uint32_t boid_map;
static int boid_active, boid_executing;

static uint32_t boid_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    /* PblHash folds every byte with OR 0x20, including underscores and digits. */
    for (; *text; ++text)
        hash = (hash ^ ((unsigned char)*text | 0x20u)) * 16777619u;
    return hash;
}

static uint32_t boid_actor(uint32_t stack, unsigned index)
{
    if (!boid_guids[index]) return 0;
    uint32_t actor = dev_call(stack, 0x001EC220u, 0, 1, &boid_guids[index]); /* GetEntityByGuid */
    return dev_guest_address(actor, 0x300) &&
           guest_u32(actor) == 0x002E0468u && /* retail RsActorStatic */
           guest_u32(actor + 4) == boid_names[index] ? actor : 0;
}

static void boid_remove(uint32_t stack)
{
    for (unsigned i = 0; i < RECOMP_BOID_COUNT; ++i) {
        uint32_t actor = boid_actor(stack, i);
        if (actor) {
            uint32_t eliminate = guest_u32(guest_u32(actor) + 0x10);
            if (recomp_lookup(eliminate)) dev_call(stack, eliminate, actor, 0, NULL);
        }
        boid_guids[i] = 0;
        boid_tracks[i] = 0;
    }
    boid_active = 0;
}

static int boid_spawn(uint32_t stack, const float *position, const float *direction)
{
    uint32_t layer = boid_hash("template_misc");
    uint32_t model = boid_hash("global_seagull");
    uint32_t template_hash = boid_hash("template_seagull");
    uint32_t lookup[] = {template_hash, 0x134603D7u}; /* geometryfile */
    if (!dev_resident_model(model) || !dev_call(stack, 0x001ED340u, 0, 2, lookup)) {
        int importing = dev_region_import_active;
        dev_region_import_active = 1;
        dev_call(stack, 0x00180AE0u, 0, 1, &layer); /* LoadList, including animation dependencies */
        if (*(uint8_t *)guest_ptr(0x00413FC9u))
            dev_call(stack, 0x0017F490u, 0, 0, NULL); /* FinishDeferredLoad */
        dev_region_import_active = importing;
    }
    if (!dev_resident_model(model) || !dev_call(stack, 0x001ED340u, 0, 2, lookup)) {
        xbox_preview_log_event("boids", "Seagull assets unavailable; spawn cancelled");
        return 0;
    }
    float x = direction[0], z = direction[2], length = sqrtf(x*x + z*z);
    if (length > .01f) { x /= length; z /= length; }
    else { x = 0; z = 1; }
    boid_vec center = {position[0] + x*25, position[1] + 3, position[2] + z*25};
    boids_init(&boid_world, center, 0xB01D51u);

    uint32_t matrix = stack + 0x140, name_text = stack + 0x200;
    uint32_t property = stack + 0x400, list = stack + 0x600;
    for (unsigned i = 0; i < RECOMP_BOID_COUNT; ++i) {
        char name[40];
        snprintf(name, sizeof(name), "recomp_boid_%02u", i);
        boid_names[i] = boid_hash(name);
        dev_call(stack, 0x001EB010u, list, 0, NULL); /* PropertyList */
        dev_property(stack, property, list, "objecttype", "static");
        dev_property(stack, property, list, "geometryfile", "global_seagull");
        dev_property(stack, property, list, "dieuponhibernation", "true");
        dev_property(stack, property, list, "name", name);
        const boid_vec p = boid_world.members[i].position;
        float mat[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, p.x,p.y,p.z,1};
        memcpy(guest_ptr(matrix), mat, sizeof(mat));
        memcpy(guest_ptr(name_text), name, strlen(name) + 1);
        /* SproutActor takes a guest string for the new name, not its hash. */
        uint32_t args[] = {template_hash, name_text, matrix, list, 0};
        uint32_t actor = dev_call(stack, 0x00174480u, 0x004031B0u, 5, args);
        if (!dev_guest_address(actor, 0x300) || guest_u32(actor) != 0x002E0468u ||
            !dev_guest_address(guest_u32(actor + 8), 0x34)) {
            xbox_preview_log_event("boids", "Spawn failed at bird %u", i);
            boid_remove(stack);
            return 0;
        }
        uint32_t spore = guest_u32(actor + 8), vtable = guest_u32(actor);
        boid_guids[i] = guest_u32(spore + 0x28);
        /* Static actors are not state-save candidates. Clear all save flags too. */
        *(uint16_t *)guest_ptr(spore + 0x30) &= (uint16_t)~0xE00u;
        uint32_t animation[] = {boid_hash("global_seagull_fly"), 10, 1}; /* idle priority, loop */
        if (!dev_call(stack, guest_u32(vtable + 0x12C), actor, 3, animation)) {
            xbox_preview_log_event("boids", "Seagull animation unavailable");
            boid_remove(stack);
            return 0;
        }
        uint32_t controller = guest_u32(actor + 0x11C);
        if (!dev_guest_address(controller, 0x50)) {
            xbox_preview_log_event("boids", "Seagull animation controller unavailable");
            boid_remove(stack);
            return 0;
        }
        /* Retail 0x60550 enables distance throttling for small models at +0x30.
         * Retail 0x606F0 can then hold a wing pose for seconds. These 15 moving
         * birds need continuous animation; other actors retain their LOD policy. */
        *(uint8_t *)guest_ptr(controller + 0x30) = 0;
    }
    boid_active = 1;
    xbox_preview_log_event("boids", "Created %u birds at %.1f,%.1f,%.1f",
                           RECOMP_BOID_COUNT, center.x, center.y, center.z);
    return 1;
}

void recomp_boids_tick(uint32_t delta_bits, uint32_t position_arg, uint32_t direction_arg)
{
    (void)position_arg;
    (void)direction_arg;
    if (boid_executing) return;
    int requested = recomp_dev_take_boids();
    if (!requested && !boid_active) return;
    const char *result = "Boids require an active outdoor province. Resume gameplay and try again.";
    int success = 0;
    if (!g_xbox_mem_offset || g_esp < 0x20000u || g_esp >= 0x4000000u ||
        guest_u32(0x413F6C) != 0x4249D707u || guest_u32(0x413F68) != 0xC2CBD863u) {
        if (requested) recomp_dev_spawn_result(0, result);
        return;
    }
    uint32_t map = guest_u32(0x403970u);
    int outdoor = map == 0x4A5220AFu || map == 0x4A364A32u;
    union { uint32_t bits; float seconds; } delta;
    delta.bits = delta_bits;
    if (!isfinite(delta.seconds) || delta.seconds < 0 || delta.seconds > 1) delta.seconds = 0;

    const uint32_t calls[] = {0x001EC220u, 0x001ED340u, 0x00180AE0u, 0x0017F490u,
                             0x001EB010u, 0x001EAFB0u, 0x001EB080u, 0x00174480u, 0x0008C7E0u};
    for (unsigned i = 0; i < sizeof(calls)/sizeof(calls[0]); ++i) {
        if (!recomp_lookup(calls[i])) {
            if (requested) recomp_dev_spawn_result(0, "Boid creation functions are unavailable.");
            return;
        }
    }
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    boid_executing = 1;
    uint32_t stack = (g_esp - 0x4000u) & ~15u;
    if (boid_map != map) {
        boid_remove(stack);
        boid_map = map;
    }
    if (!outdoor) { boid_remove(stack); goto done; }
    /* Reloads invalidate GUIDs even when the province hash is unchanged.
     * Only a new button press creates a flock; never respawn it automatically. */
    if (boid_active && !boid_actor(stack, 0)) boid_remove(stack);
    if (requested) {
        uint32_t name = 0x660E4490u; /* player0 */
        uint32_t controller = dev_call(stack, 0x0008C7E0u, 0, 1, &name);
        uint32_t player = dev_player_human(controller);
        result = "Player position is unavailable. Resume gameplay and try again.";
        if (!dev_guest_address(player, 0x76C)) goto done;
        uint32_t vtable = guest_u32(player);
        if (!dev_guest_address(vtable, 0x40) || !recomp_lookup(guest_u32(vtable + 0x34))) goto done;
        uint32_t position_buffer = stack + 0x100;
        dev_call(stack, guest_u32(vtable + 0x34), player, 1, &position_buffer);
        float position[3], direction[3] = {0, 0, 1};
        memcpy(position, guest_ptr(position_buffer), sizeof(position));
        for (unsigned i = 0; i < 3; ++i) if (!isfinite(position[i])) goto done;
        /* Anchor to the human, including in free cam; use the actor's heading. */
        direction[0] = -*(float *)guest_ptr(player + 0xD0);
        direction[2] = -*(float *)guest_ptr(player + 0xD8);
        if (!isfinite(direction[0]) || !isfinite(direction[2])) goto done;
        boid_remove(stack);
        success = boid_spawn(stack, position, direction);
        result = success ? "Created 15 birds near the player. Click again to replace the flock."
                         : "Could not create the flock. See the preview log for details.";
    }
    if (boid_active) {
        unsigned moved = boids_advance(&boid_world, delta.seconds);
        for (unsigned i = 0; i < RECOMP_BOID_COUNT; ++i) {
            uint32_t actor = boid_actor(stack, i);
            if (!actor) continue;
            uint32_t controller=guest_u32(actor+0x11C);
            if (delta.seconds>0 && dev_guest_address(controller,0x50) && guest_u32(controller+4)==1) {
                uint32_t track=guest_u32(controller+8);
                if (dev_guest_address(track,0xB50)) {
                    float phase=*(float *)guest_ptr(track+0xB20);
                    float rate=*(float *)guest_ptr(track+0xB3C);
                    if (boid_tracks[i]!=track && isfinite(phase) && phase>=0 && phase<=1 && isfinite(rate) && rate>0) {
                        boid_tracks[i]=track;boid_track_rates[i]=rate;
                        boid_wings_init(&boid_wing_states[i],i,phase);
                    }
                    if (boid_tracks[i]==track) {
                        int glide=boid_wings_advance(&boid_wing_states[i],delta.seconds,phase);
                        /* Retail 0x62F70 scales dt by +B3C; 0x206D60 advances
                         * normalized phase at +B20. Only this bird's track changes. */
                        *(float *)guest_ptr(track+0xB3C)=glide?0:boid_track_rates[i];
                        if (glide) *(float *)guest_ptr(track+0xB20)=BOID_GLIDE_PHASE;
                    }
                }
            }
            if (!moved) continue;
            boid_member *bird = &boid_world.members[i];
            float length = sqrtf(bird->velocity.x*bird->velocity.x + bird->velocity.z*bird->velocity.z);
            if (length > .01f) {
                /* RedActor's forward direction is negative Z. Birds stay level. */
                float x = -bird->velocity.x/length, z = -bird->velocity.z/length;
                float rotation[12] = {z,0,-x,0, 0,1,0,0, x,0,z,0};
                memcpy(guest_ptr(actor + 0xB0), rotation, sizeof(rotation));
            }
            uint32_t position = stack + 0x100;
            memcpy(guest_ptr(position), &bird->position, 12);
            dev_call(stack, guest_u32(guest_u32(actor) + 0x64), actor, 1, &position); /* SetPosition */
            uint32_t spore = guest_u32(actor + 8);
            if (dev_guest_address(spore, 0x34))
                *(uint16_t *)guest_ptr(spore + 0x30) &= (uint16_t)~0xE00u;
        }
    }
done:
    boid_executing = 0;
    recomp_restore_guest_cpu_context(&saved);
    if (requested) recomp_dev_spawn_result(success, result);
}
