#ifndef MERCENARIES_DEV_MENU_H
#define MERCENARIES_DEV_MENU_H
#include <windows.h>
#include <stdint.h>
typedef struct DevVehicle {
    const char *name, *template_name, *faction, *kind;
    uint32_t template_hash, model_hash;
    float radius, bottom, height, half_width, half_length;
    const char *asset_list;
} DevVehicle;
void recomp_dev_menu_init(HWND owner, void (*bookmark)(void));
int recomp_dev_menu_allowed(void);
int recomp_developer_logging_enabled(void);
void recomp_dev_menu_set_display_callback(void (*callback)(int));
void recomp_dev_menu_toggle(void);
void recomp_dev_freecam_toggle(void);
int recomp_dev_menu_visible(void);
int recomp_dev_menu_camera_input(int keyboard);
const DevVehicle *recomp_dev_vehicle_at(unsigned index);
unsigned recomp_dev_vehicle_count(void);
int recomp_dev_take_spawn(unsigned *index);
int recomp_dev_take_boids(void);
void recomp_dev_request_boids(void);
void recomp_dev_spawn_result(int success, const char *message);
/* Consumed only at the retail main-thread world-update boundary. */
void recomp_dev_spawn_tick(void);
void recomp_dev_spawn_progress(const char *message);
#endif
