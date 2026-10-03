#ifndef MERCENARIES_DEV_WEAPONS_H
#define MERCENARIES_DEV_WEAPONS_H
#include <stddef.h>
/* Handheld firearms from retail template_weapons; zero preserves the troop's loadout. */
typedef struct DevWeapon { const char *name, *template_name; } DevWeapon;
static const DevWeapon dev_weapons[] = {
    {"Default loadout", NULL},
    {"Assault Rifle (AK-47)", "template_pic_ak47"},
    {"Carbine (M4)", "template_pic_m4"},
    {"SMG (Type 85)", "template_pic_smg"},
    {"Covert SMG (MP5)", "template_pic_mp5"},
    {"Shotgun", "template_pic_shotgun"},
    {"Light Machine Gun (RPD)", "template_pic_lmg"},
    {"Machine Gun (MG36)", "template_pic_mg36"},
    {"Sniper Rifle", "template_pic_sniperrifle"},
    {"Covert Sniper Rifle", "template_pic_covertrifle"},
    {"Anti-Materiel Rifle", "template_pic_amrifle"},
    {"RPG", "template_pic_rpg"},
    {"SMAW", "template_pic_smaw"},
    {"Anti-Air Missile (Stinger)", "template_pic_stinger"},
    {"Cheat SMG", "template_pic_cheatsmg"},
    {"Cheat Grenade Launcher", "template_pic_cheatgl"},
    {"Cheat Gun", "template_pic_cheatgun"}
};
#define DEV_WEAPON_COUNT (sizeof(dev_weapons) / sizeof(dev_weapons[0]))
#endif
