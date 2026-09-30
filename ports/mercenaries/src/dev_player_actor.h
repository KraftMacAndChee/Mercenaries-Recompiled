/* Retail 0x8C7E0 returns the player controller, not the human actor.
 * Its constructor installs vtable 0x2E65E0; slot +0x34 is 0x8C050,
 * which returns controller+0x998. See retail-player-actor-evidence.md. */
#ifndef MERCENARIES_DEV_PLAYER_ACTOR_H
#define MERCENARIES_DEV_PLAYER_ACTOR_H
static uint32_t dev_player_human(uint32_t controller)
{
    if (!dev_guest_address(controller, 0x99C)) return 0;
    uint32_t human = guest_u32(controller + 0x998);
    return dev_guest_address(human, 0x76C) ? human : 0;
}
#endif
