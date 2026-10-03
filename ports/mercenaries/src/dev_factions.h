#ifndef MERCENARIES_DEV_FACTIONS_H
#define MERCENARIES_DEV_FACTIONS_H
/* Retail 0x00095CA0 maps these indices to prokat, nk, sk, mafia, china, allies, civ. */
static const char *const dev_faction_names[] = {
    "Player / Mercenaries", "North Korean", "South Korean", "Russian Mafia",
    "Chinese", "Allied Nations", "Civilian"
};
static const char *const dev_relation_names[] = {"Hostile", "Unfriendly", "Neutral", "Friendly"};
/* Representative standings for the four attitudes classified by retail 0x00095EE0. */
static const float dev_relation_values[] = {-1.f, -.4f, 0.f, 1.f};
static unsigned dev_relation_command(unsigned first, unsigned second, unsigned relation)
{
    if (first >= 7u || second >= 7u || first == second || relation >= 4u) return 0;
    return 0x100u | first | (second << 3u) | (relation << 6u);
}
int recomp_dev_request_relation(unsigned first, unsigned second, unsigned relation);
unsigned recomp_dev_take_relation(void);
void recomp_dev_relation_result(int success, const char *message);
void recomp_dev_relations_tick(void);
#endif
