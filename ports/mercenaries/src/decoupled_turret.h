#ifndef MERCENARIES_DECOUPLED_TURRET_H
#define MERCENARIES_DECOUPLED_TURRET_H
#include <stdint.h>
/* Decoupled turrets (tank main guns, gunner seats, emplacements).
 *
 * Retail RsCameraStateTurret welds the camera to the barrel and the mouse sets
 * the turret's traverse rate. With this option the mouse turns a free camera
 * (world yaw/pitch); the turret slews toward the camera's aim point at its
 * authored YawSpeed/PitchSpeed, and fires along its barrel. A controller
 * stick or turret key takes back retail control while it is deflected.
 *
 * This module only holds host-side state; decoupled_turret_guest.h performs
 * the guest-memory and guest-call work from recomp_manual.c. */
void decoupled_turret_load(const char *ini_path);
void decoupled_turret_save(const char *ini_path);
int decoupled_turret_enabled(void);
void decoupled_turret_set_enabled(int enabled);
/* Mouse look in a turret context: camera angles in radians (x right, y up). */
void decoupled_turret_mouse(float yaw_angle, float pitch_angle);
/* A deflected controller stick hands control back to the retail turret. */
void decoupled_turret_controller_input(void);
/* The turret camera rendered this turret this frame. */
void decoupled_turret_note_camera(uint32_t turret);
uint32_t decoupled_turret_camera_turret(void);
/* Free-camera direction; returns 0 when it must be re-seeded from the view. */
int decoupled_turret_direction(float forward[3]);
void decoupled_turret_seed(const float forward[3]);
/* Apply pending mouse angles using the camera's current screen-right axis. */
void decoupled_turret_apply_input(const float camera_right[3]);
int recomp_decoupled_turret_live(void);

/* Helicopter pilots (Decoupled Helicopters option). The mouse turns the chase camera's world
 * yaw instead of the helicopter; the helicopter turns toward it with the
 * retail AI rule (RsAiPlayer::TurnTo: steer = yaw error * pi), and the
 * player's helicopter turrets slew to the aim point. Retail vertical look is
 * unchanged. */
int decoupled_heli_enabled(void);
void decoupled_heli_set_enabled(int enabled);
void decoupled_heli_mouse(float yaw_angle);
void decoupled_heli_controller_input(void);
/* Chase-camera update: heading_yaw is the "behind" yaw the retail camera
 * would snap to; right_turn is +1/-1, the sense in which a rightward turn
 * changes yaw for this camera. Returns 1 and the free yaw while live. */
int decoupled_heli_camera(uint32_t vehicle, float heading_yaw, float right_turn, float *yaw);
/* Free yaw to apply last in the chase-camera update; 0 while not live. */
int decoupled_heli_free_yaw(float *yaw);
/* Turn input for the helicopter (logical turn axis, -1..1). */
float decoupled_heli_steer(void);
/* RsAiPlayer aims these helicopter turrets for the player this frame. */
void decoupled_heli_note_turret(uint32_t turret);
int decoupled_heli_turret(uint32_t turret);
int recomp_decoupled_heli_live(void);
/* Either mode is driving the player's weapon this frame. */
int recomp_decoupled_vehicle_live(void);
#endif
