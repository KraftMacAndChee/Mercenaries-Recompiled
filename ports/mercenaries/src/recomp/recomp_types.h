/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * This port also declares Mercenaries compatibility hooks. Generated
 * functions, manual replacements, and the kernel bridge share this ABI.
 *
 * Memory model:
 *   Guest-memory accesses translate Xbox VAs through g_xbox_mem_offset.
 *   The port supplies the mapping and initializes that offset before
 *   guest execution. Native C executes the translated instructions.
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   through global registers and the simulated Xbox stack.
 *   Return values are communicated through g_eax.
 *   The call instruction pushes a dummy return address; ret pops it.
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include "../mod_extensions.h"
int recomp_controls_mouse_look_pending(void);
float recomp_turbulence_damping_dt(float dt);
int recomp_static_light_refresh_needed(uint32_t light);
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. On Windows the
 * MinGW headers define __forceinline as "extern __inline__ ...", which
 * conflicts with the "static __forceinline" used below, so override it. */
#if !defined(_MSC_VER)
#undef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions.
 * On Windows the SDK/MinGW already provides __debugbreak. */
#if !defined(_WIN32)
#if !defined(__debugbreak)
#define __debugbreak() __builtin_trap()
#endif
#endif

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Guest-to-host address translation: host_address = xbox_va + offset.
 * Set during memory initialization before guest execution, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* ================================================================
 * Global registers
 * ================================================================ */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to top of Xbox stack)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - global because callers pass implicit parameters
 *   through them. The callee-save contract is enforced by generated
 *   PUSH32/POP32 instructions.
 *
 * NOT global: ebp - stays local in each function because FPO
 * functions use it as scratch. For SEH, g_seh_ebp bridges the gap.
 */
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_ebx, g_esi, g_edi;
extern volatile uint32_t g_recomp_14fa2b_entry;

/** x87 register stack, shared across translated function calls. */
extern double g_fp_stack[8];
extern uint32_t g_fp_top;
extern uint16_t g_x87_control_word;
extern uint16_t g_x87_status_word;

/** SSE and MMX architectural registers, shared across translated calls. */
extern float g_xmm0[4], g_xmm1[4], g_xmm2[4], g_xmm3[4];
extern float g_xmm4[4], g_xmm5[4], g_xmm6[4], g_xmm7[4];
extern uint64_t g_mm0, g_mm1, g_mm2, g_mm3;
extern uint64_t g_mm4, g_mm5, g_mm6, g_mm7;

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog sets up ebp for the caller, but since ebp is a local
 * variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
extern uint32_t g_seh_ebp;

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
void recomp_icall_fail_log(uint32_t va);
void recomp_pose_traversal_overflow(uint32_t pose, uint32_t joint,
                                    uint32_t count, uint32_t next_joint);
void recomp_xbox_debug_print(uint32_t text_address, uint32_t text_length);
void recomp_arm_transition_pool_watchpoint(uint32_t guest_va);
void recomp_transition_pool_reset_checkpoint(uint32_t guest_va);
void recomp_transition_this_checkpoint(uint32_t stage, uint32_t expected,
                                       uint32_t actual);
void recomp_transition_callee_checkpoint(uint32_t stage,
                                         uint32_t expected_esp,
                                         uint32_t actual_esp,
                                         uint32_t expected_esi,
                                         uint32_t saved_esi,
                                         uint32_t pending_esi);
void recomp_xinput_init_devices(void);
uint32_t recomp_xinput_get_devices(void);
uint32_t recomp_xinput_get_device_changes(uint32_t insertions_address,
                                           uint32_t removals_address);
uint32_t recomp_xinput_open(uint32_t port);
void recomp_xinput_close(uint32_t handle);
uint32_t recomp_xinput_get_capabilities(uint32_t handle,
                                         uint32_t capabilities_address);
uint32_t recomp_xinput_get_state(uint32_t handle, uint32_t state_address);
uint32_t recomp_xinput_set_state(uint32_t handle, uint32_t feedback_address);
void recomp_input_logical_checkpoint(uint32_t joystick_address);
int recomp_dev_region_import_active(void);
int recomp_stream_admit_idle(uint32_t manager,uint32_t bytes);
int recomp_controls_aim_assist(void);
float recomp_controls_mouse_delta(unsigned axis, float original);
float recomp_controls_mouse_axis(uint32_t hash, float original, float dt);
void recomp_player_control_checkpoint(uint32_t stage, uint32_t player,
                                      uint32_t control, uint32_t value_bits,
                                      uint32_t actor);
void recomp_player_vehicle_exit_checkpoint(uint32_t stage,
                                           uint32_t player_ai);
void recomp_human_collision_checkpoint(uint32_t stage, uint32_t actor,
                                       uint32_t enable,
                                       uint32_t obstacle);
void recomp_inventory_item_checkpoint(uint32_t stage, uint32_t owner,
                                      uint32_t item, uint32_t dock,
                                      uint32_t renderable,
                                      uint32_t matrix);
void recomp_weapon_render_toggle_checkpoint(uint32_t stage, uint32_t owner,
                                            uint32_t enable);
void recomp_player_loadout_checkpoint(uint32_t stage, uint32_t owner,
                                      uint32_t template_hash,
                                      uint32_t value);
void recomp_mopp_linear_cast_checkpoint(uint32_t stage, uint32_t phantom,
                                        uint32_t self_type,
                                        uint32_t other_type,
                                        uint32_t collidable,
                                        uint32_t target,
                                        uint32_t input,
                                        uint32_t cast_collector,
                                        uint32_t start_collector);
void recomp_mopp_vm_hit_checkpoint(uint32_t stage, uint32_t machine,
                                   uint32_t shape_key,
                                   uint32_t allowed,
                                   uint32_t child_shape,
                                   uint32_t child_type,
                                   uint32_t target,
                                   uint32_t cast_collector,
                                   uint32_t start_collector);
void recomp_havok_linear_cast_checkpoint(uint32_t stage, uint32_t body_a,
                                         uint32_t body_b, uint32_t input,
                                         uint32_t cast_collector,
                                         uint32_t start_collector,
                                         uint32_t target);
void recomp_havok_iterative_checkpoint(uint32_t body_a, uint32_t body_b,
                                       uint32_t input, uint32_t collector,
                                       uint32_t closest_output,
                                       uint32_t closest_target);
void recomp_havok_box_feature_checkpoint(uint32_t result,
                                         uint32_t detector,
                                         uint32_t stack,
                                         uint32_t work,
                                         uint32_t feature,
                                         uint32_t output);
void recomp_mopp_long_ray_return_checkpoint(
    uint32_t machine, uint32_t hidden_result, uint32_t frame,
    uint32_t stack_after, uint32_t ebx_before, uint32_t esi_before,
    uint32_t edi_before, uint32_t ebx_after, uint32_t esi_after,
    uint32_t edi_after);
void recomp_mopp_opcode_checkpoint(uint32_t program, uint32_t opcode,
                                   uint32_t mapped_index, uint32_t target,
                                   uint32_t stack, uint32_t frame);
void recomp_camera_collision_checkpoint(uint32_t site, uint32_t state,
                                        uint32_t result);
void recomp_camera_post_collision_checkpoint(uint32_t site, uint32_t state,
                                             uint32_t focus,
                                             uint32_t direction,
                                             uint32_t new_length_bits);
void recomp_camera_repair_direction(uint32_t state, uint32_t direction);
void recomp_camera_pre_collision_checkpoint(uint32_t state,
                                            uint32_t camera_position,
                                            uint32_t direction);
void recomp_camera_collision_query_checkpoint(uint32_t stage,
                                              uint32_t result,
                                              uint32_t camera_position);
uint32_t recomp_ai_perception_ray_count_checkpoint(uint32_t site,
                                                   uint32_t candidate_count,
                                                   uint32_t stack);
uint32_t recomp_camera_ray_callsite_checkpoint(uint32_t site, uint32_t ray,
                                           uint32_t candidate_count,
                                           uint32_t stack);
uint32_t recomp_camera_ray_begin_checkpoint(uint32_t ray, uint32_t flags,
                                            uint32_t include_flags,
                                            uint32_t candidate_count,
                                            uint32_t frame);
void recomp_camera_ray_candidate_checkpoint(uint32_t broadphase_handle);
void recomp_camera_ray_shape_result_checkpoint(uint32_t collidable,
                                                uint32_t result_address,
                                                uint32_t output,
                                                uint32_t shape);
void recomp_camera_mode_checkpoint(uint32_t setter);
void recomp_camera_mode_post_checkpoint(uint32_t setter);
void recomp_radius_collection_checkpoint(uint32_t center, uint32_t radius_bits,
                                         uint32_t include_flags,
                                         uint32_t excluded,
                                         uint32_t broadphase_count);
void recomp_briefing_actor_checkpoint(uint32_t stage,
                                      uint32_t template_name,
                                      uint32_t actor_name,
                                      uint32_t hardpoint,
                                      uint32_t actor);
void recomp_broadphase_query_checkpoint(uint32_t broadphase,
                                        uint32_t min_x, uint32_t min_y,
                                        uint32_t min_z, uint32_t max_x,
                                        uint32_t max_y, uint32_t max_z);
void recomp_broadphase_bits_checkpoint(uint32_t broadphase,
                                       uint32_t bitfield,
                                       uint32_t packed_min_yz,
                                       uint32_t packed_max_yz);
void recomp_frontend_menu_checkpoint(uint32_t owner_address,
                                      uint32_t new_menu_address);
void recomp_main_state_checkpoint(uint32_t state, uint32_t substate);
void recomp_frontend_shell_loop_checkpoint(uint32_t stage, uint32_t owner,
                                           uint32_t channel);
void recomp_frontend_input_checkpoint(uint32_t menu_address,
                                       uint32_t input, uint32_t event);
uint32_t recomp_options_input_checkpoint(uint32_t menu_address,
                                         uint32_t input, uint32_t event);
uint32_t recomp_options_localization_hash(uint32_t hash);
int recomp_options_authentic_haze(void);
  float recomp_options_scale_draw_distance(float distance);
  float recomp_options_scale_camera_visibility(float visibility);
  float recomp_options_scale_ai_visibility_threshold(float threshold);
float recomp_options_scale_ai_behind_distance_squared(float distance_squared);
float recomp_options_scale_npc_draw_distance(float distance,
                                             int lod_adjustment_type);
float recomp_options_scale_ambient_civ_distance(float distance);
int recomp_options_force_high_npc_lod(int lod_adjustment_type);
uint32_t recomp_options_high_npc_lod_mask(uint32_t mask);
float recomp_options_scale_object_distance(float distance, uint32_t type);
float recomp_options_object_distance_multiplier(void);
float recomp_options_perspective_fov(float horizontal_fov,
                                     float guest_aspect);
float recomp_options_perspective_aspect(float guest_aspect);
float recomp_options_satellite_center_x(float x);
float recomp_options_satellite_center_width(float width);
float recomp_options_ui_x(float x, float anchor);
void recomp_ui_record_position(uint32_t brush, float x, uint32_t screen_ref);
void recomp_ui_begin_brush(uint32_t brush);
void recomp_ui_end_brush(void);
uint32_t recomp_renderer_test_oom(uint32_t multisample);
void recomp_renderer_recovery_report(uint32_t result, uint32_t stage);
float recomp_options_satellite_center_x(float x);
float recomp_options_satellite_center_width(float width);
void recomp_loadsave_state_checkpoint(uint32_t stage, uint32_t owner_address,
                                          uint32_t state_address);
uint32_t recomp_loadsave_callback_sanitize(uint32_t owner_address,
                                           uint32_t callback_address);
void recomp_splash_d3d_checkpoint(uint32_t phase);
void recomp_d3d_sync_checkpoint(uint32_t phase);
void recomp_redspace_bounds_checkpoint(
    uint32_t space_level, uint32_t circle, int32_t min_grid_x,
    int32_t min_grid_z, int32_t max_grid_x, int32_t max_grid_z,
    int32_t total_grid_squares, int32_t lower_bound, int32_t upper_bound,
    int32_t grid_width);
void recomp_redspace_probe_checkpoint(
    uint32_t space_level, int32_t grid_counter, int32_t grid_z,
    uint32_t index, uint32_t item);
void recomp_redspace_query_checkpoint(uint32_t stage, uint32_t space,
                                      uint32_t result, uint32_t count_or_max,
                                      uint32_t saved_count);
void recomp_actor_dispatch_checkpoint(uint32_t return_address,
                                      uint32_t manager,
                                      uint32_t object,
                                      uint32_t guest_stack);
uint32_t recomp_havok_callback_count_checkpoint(uint32_t owner,
                                                uint32_t array,
                                                uint32_t count,
                                                uint32_t context,
                                                uint32_t guest_stack);
void recomp_actor_init_checkpoint(uint32_t stage, uint32_t object,
                                  uint32_t argument1,
                                  uint32_t argument2);
void recomp_ai_init_stack_checkpoint(uint32_t stage, uint32_t owner,
                                     uint32_t guest_stack,
                                     uint32_t saved_esi,
                                     uint32_t saved_ebx);
void recomp_ai_base_init_checkpoint(uint32_t stage, uint32_t owner,
                                    uint32_t value1, uint32_t value2,
                                    uint32_t guest_stack);
uint32_t recomp_ai_process_stimuli_esi_checkpoint(uint32_t expected,
                                                  uint32_t actual);
uint32_t recomp_ai_process_stimuli_esp_checkpoint(uint32_t expected,
                                                  uint32_t actual);
uint32_t recomp_ai_process_stimuli_frame_checkpoint(uint32_t site,
                                                    uint32_t expected,
                                                    uint32_t actual);
uint32_t recomp_ai_update_vtable_checkpoint(uint32_t site, uint32_t object,
                                            uint32_t expected_vtable);
uint32_t recomp_ai_stimulus_boundary_checkpoint(uint32_t stage, uint32_t object,
                                            uint32_t expected_vtable,
                                            uint32_t expected_depth);
int recomp_spore_ppd_is_valid(uint32_t site, uint32_t spore,
                              uint32_t ppd, uint32_t key);
void recomp_spore_predicate_checkpoint(uint32_t stage, uint32_t spore,
                                        uint32_t camera, uint32_t position);
void recomp_redscene_checkpoint(uint32_t stage, uint32_t scene,
                                uint32_t subject, uint32_t result_buffer,
                                uint32_t count);
void recomp_terrain_rendering_request(uint32_t requested_enable,
                                      uint32_t renderable_flags);
uint32_t recomp_redscene_collected_item_checkpoint(
    uint32_t level, uint32_t source_index, uint32_t item,
    uint32_t result_index);
uint32_t recomp_actor_query_entry_checkpoint(uint32_t result_index,
                                             uint32_t spatial_item,
                                             uint32_t actor);
uint32_t recomp_actor_query_count_checkpoint(uint32_t site, uint32_t count);
void recomp_nonvolatile_icall_checkpoint(
    uint32_t site, uint32_t expected_ebx, uint32_t current_ebx,
    uint32_t expected_esi, uint32_t current_esi,
    uint32_t expected_edi, uint32_t current_edi,
    uint32_t expected_ebp, uint32_t current_ebp,
    uint32_t expected_esp, uint32_t current_esp);
void recomp_weapon_fire_sound_checkpoint(
    uint32_t stage, uint32_t effect, uint32_t detail, uint32_t weapon,
    uint32_t cue_or_result, uint32_t handle, uint32_t stack);
void recomp_human_fire_anim_checkpoint(uint32_t human, uint32_t anim,
                                       float pitch, uint32_t shoot_state,
                                       uint32_t shoot_anim);
void recomp_bullet_hit_checkpoint(uint32_t stage, uint32_t projectile,
                                  uint32_t actor, uint32_t value);
void recomp_bullet_damage_result_checkpoint(uint32_t projectile,
                                            uint32_t actor, float result);
void recomp_human_fire_play_checkpoint(
    uint32_t stage, uint32_t human, uint32_t anim, uint32_t handle,
    uint32_t params, uint32_t shoot_state, uint32_t shoot_anim,
    uint32_t result);
void recomp_human_weapon_update_checkpoint(
    uint32_t stage, uint32_t human, uint32_t weapon,
    uint32_t value0, uint32_t value1);
void recomp_menu_paint_checkpoint(uint32_t stage, uint32_t menu,
                                  uint32_t index, uint32_t item_hash,
                                  uint32_t fade_bits, uint32_t text_buffer);
void recomp_shell_prewarm_checkpoint(uint32_t iteration,
                                     uint32_t position_address,
                                     uint32_t direction_x_bits,
                                     uint32_t direction_y_bits,
                                     uint32_t direction_z_bits);
void recomp_world_xfrm_checkpoint(uint32_t matrix_address,
                                   uint32_t chunk_address);
void recomp_asset_dispatch_checkpoint(uint32_t asset_name,
                                      uint32_t asset_type,
                                      uint32_t chunk_address);
void recomp_world_transform_checkpoint(uint32_t matrix_address,
                                        uint32_t transform_address);
void recomp_world_composed_checkpoint(uint32_t kind, uint32_t matrix_address,
                                      uint32_t transform_address);
void recomp_temp_array_checkpoint(uint32_t stage, uint32_t object,
                                  uint32_t allocator, uint32_t value0,
                                  uint32_t value1);
void recomp_allocator_alias_checkpoint(uint32_t site, uint32_t target);
void recomp_allocator_call_checkpoint(uint32_t site, uint32_t phase,
                                      uint32_t allocator,
                                      uint32_t requested,
                                      uint32_t result);
void recomp_lua_buffer_grow_checkpoint(uint32_t buffer, uint32_t desired,
                                       uint32_t current);
void recomp_lua_settable_checkpoint(uint32_t state, uint32_t iteration,
                                    uint32_t target, uint32_t metamethod,
                                    uint32_t key, uint32_t value);
void recomp_shl_vm_checkpoint(uint32_t stage, uint32_t state,
                              uint32_t value0, uint32_t value1);
void recomp_lua_equal_checkpoint(uint32_t left, uint32_t right,
                                 uint32_t result, uint32_t expected,
                                 uint32_t instruction, uint32_t next_pc);
void recomp_update_caller_checkpoint(uint32_t caller, uint32_t object);
void recomp_update_esi_checkpoint(uint32_t site, uint32_t expected,
                                  uint32_t current);
void recomp_object_getter_checkpoint(uint32_t site, uint32_t object,
                                     uint32_t return_address);
uint32_t recomp_validate_texture_bind(uint32_t caller, uint32_t stage,
                                      uint32_t texture);
void recomp_transient_update_abi_checkpoint(
    uint32_t site, uint32_t expected_esi, uint32_t current_esi,
    uint32_t expected_edi, uint32_t current_edi,
    uint32_t expected_esp, uint32_t current_esp);
void recomp_human_animation_pointer_checkpoint(uint32_t stage,
                                               uint32_t actor);
void recomp_zephyr_anim_checkpoint(uint32_t stage, uint32_t instance,
                                    uint32_t source_handle,
                                    uint32_t animation);
void recomp_collision_agent_checkpoint(uint32_t stage, uint32_t object,
                                       uint32_t context, uint32_t detail);
void recomp_vehicle_door_checkpoint(uint32_t stage, uint32_t object,
                                    uint32_t context, uint32_t detail);
extern volatile uint32_t g_recomp_collision_agent_watch_enabled;
void recomp_collision_agent_watch_checkpoint(uint32_t xbox_va);
void recomp_collision_agent_recycle_range(uint32_t base, uint32_t size,
                                          uint32_t site);
void recomp_collision_manager_checkpoint(uint32_t manager);
uint32_t recomp_collision_dispatch_state(uint32_t object, uint32_t state);
void recomp_human_move_selection_checkpoint(uint32_t physics,
                                            float stick_magnitude);
void recomp_datapod_timer_checkpoint(uint32_t vm, float delta_time,
                                     float input_timer);
void recomp_secondary_ammo_checkpoint(uint32_t stage, uint32_t hud,
                                      uint32_t value);
void recomp_human_jump_checkpoint(uint32_t stage, uint32_t actor,
                                  uint32_t physics, uint32_t result);
void recomp_human_run_enabled_checkpoint(uint32_t site, uint32_t actor,
                                         uint32_t physics, uint32_t enabled);
extern volatile uint32_t g_recomp_target_manager_watch_enabled;
void recomp_target_manager_checkpoint(uint32_t stage, uint32_t manager,
                                      uint32_t target);
void recomp_target_manager_watch_checkpoint(uint32_t xbox_va);
extern volatile uint32_t g_recomp_particle_list_watch_enabled;
void recomp_particle_list_watch_checkpoint(uint32_t xbox_va);
void recomp_human_head_checkpoint(uint32_t stage, uint32_t animation,
                                  uint32_t dt_bits);
void recomp_redmodel_render_checkpoint(uint32_t stage, uint32_t object,
                                       uint32_t value0, uint32_t value1);
void recomp_redmodel_queue_checkpoint(
    uint32_t stage, uint32_t phase, uint32_t shader_id,
    uint32_t item, uint32_t shader_object, uint32_t target,
    uint32_t expected_esi, uint32_t current_esi,
    uint32_t expected_edi, uint32_t current_edi,
    uint32_t expected_esp, uint32_t current_esp);
void recomp_redprimitive_draw_checkpoint(uint32_t stage,
                                         uint32_t primitive);
void recomp_player_update_lifetime_checkpoint(uint32_t stage,
                                              uint32_t owner);
void recomp_notification_list_checkpoint(uint32_t stage, uint32_t cursor,
                                         uint32_t entry, uint32_t target);
void recomp_notification_owner_watch_checkpoint(uint32_t xbox_va);
uint32_t recomp_notification_cursor_sanitize(uint32_t cursor);
uint32_t recomp_pbl_thread_next_after_update(uint32_t cursor,
                                             uint32_t saved_next);
void recomp_vehicle_reward_checkpoint(uint32_t stage, uint32_t actor,
                                      uint32_t source, uint32_t credited,
                                      uint32_t cash_bits,
                                      uint32_t money_bits);
void recomp_find_culprit_checkpoint(uint32_t victim, uint32_t damage_type,
                                    uint32_t suspect_mask, uint32_t culprit,
                                    uint32_t best_value_bits);
void recomp_camera_tilt_loop_checkpoint(uint32_t site,
                                        uint32_t expected_edi,
                                        uint32_t current_edi,
                                        uint32_t expected_esp,
                                        uint32_t current_esp);
void recomp_havok_constraint_esi_checkpoint(uint32_t constraint,
                                            uint32_t target,
                                            uint32_t expected,
                                            uint32_t current);
void recomp_havok_nested_abi_checkpoint(uint32_t stage, uint32_t target,
                                        uint32_t expected_esi,
                                        uint32_t current_esi,
                                        uint32_t saved_slot,
                                        uint32_t expected_slot,
                                        uint32_t current_slot,
                                        uint32_t expected_esp,
                                        uint32_t current_esp);
void recomp_gate_ai_checkpoint(uint32_t stage, uint32_t gate_ai,
                               uint32_t gate_actor, uint32_t stimulus,
                               uint32_t stimulus_actor, float value0,
                               float value1);
void recomp_gate_motion_checkpoint(uint32_t stage, uint32_t gate_actor,
                                   uint32_t gate_index, uint32_t gate_prop,
                                   uint32_t physics, float value0,
                                   float value1, float value2);
void recomp_tree_predicate_checkpoint(uint32_t stage, uint32_t object,
                                      uint32_t out, uint32_t left,
                                      uint32_t right);
void recomp_tree_merge_checkpoint(uint32_t stage, uint32_t entry,
                                  uint32_t table, uint32_t index,
                                  uint32_t left, uint32_t right);
void recomp_pair_sort_checkpoint(uint32_t base, uint32_t count,
                                 uint32_t stack);
void recomp_pair_append_checkpoint(uint32_t site, uint32_t first,
                                   uint32_t second, uint32_t array,
                                   uint32_t stack, uint32_t frame);
void recomp_broadphase_invariant_checkpoint(uint32_t context,
                                            uint32_t object_cursor,
                                            uint32_t object_end,
                                            uint32_t aabb_cursor);
void recomp_render_owner_checkpoint(uint32_t phase, uint32_t object);
void recomp_render_list_checkpoint(uint32_t phase, uint32_t root,
                                   uint32_t node);
void recomp_render_stack_checkpoint(uint32_t phase, uint32_t saved_esp,
                                    uint32_t current_esp,
                                    uint32_t saved_esi,
                                    uint32_t stack_value);
void recomp_render_icall_checkpoint(uint32_t phase, uint32_t target,
                                    uint32_t saved_esp,
                                    uint32_t current_esp);
void recomp_pda_store_render_checkpoint(uint32_t phase, uint32_t view,
                                        uint32_t category, uint32_t item,
                                        uint32_t row, uint32_t value,
                                        uint32_t stack);
void recomp_d3d_device_checkpoint(uint32_t phase, uint32_t expected,
                                  uint32_t actual, uint32_t stack);
void recomp_lua_rehash_checkpoint(uint32_t result, uint32_t table,
                                  uint32_t key);
void recomp_lua_checkcode_checkpoint(void);
void recomp_lua_symbexec_checkpoint(void);
uint32_t recomp_lua_symbexec_args_valid(uint32_t proto, uint32_t lastpc);
void recomp_lua_typeerror_checkpoint(void);
void *recomp_lua_host_jmp_register(uint32_t guest_buffer);
uint32_t recomp_lua_host_longjmp(uint32_t guest_buffer, uint32_t value);
void recomp_lua_host_jmp_pop(uint32_t guest_buffer);
#define RECOMP_LUA_HOST_SETJMP(guest_buffer) \
    setjmp(*(jmp_buf *)recomp_lua_host_jmp_register((guest_buffer)))
double recomp_native_strtod(uint32_t guest_string,
                            uint32_t guest_end_pointer);
uint32_t recomp_native_format_lua_number(uint32_t guest_buffer,
                                         double value);
void recomp_lua_callframe_checkpoint(uint32_t stage, uint32_t state,
                                     uint32_t function_object,
                                     uint32_t aux0, uint32_t aux1);
void recomp_lua_poscall_site_checkpoint(uint32_t site, uint32_t state,
                                        uint32_t wanted,
                                        uint32_t first_result);
void recomp_lua_vm_precall_checkpoint(uint32_t stage, uint32_t state,
                                      uint32_t function_object,
                                      uint32_t result_or_wanted,
                                      uint32_t stack, uint32_t saved_wanted);
void recomp_lua_callback_117830_checkpoint(uint32_t stage, uint32_t target,
                                           uint32_t saved_stack,
                                           uint32_t current_stack);
void recomp_lua_cclosure_return_checkpoint(uint32_t target,
                                           uint32_t saved_stack,
                                           uint32_t current_stack,
                                           uint32_t state);
void recomp_lua_precall_return_checkpoint(uint32_t saved_stack,
                                          uint32_t current_stack,
                                          uint32_t state,
                                          uint32_t restored_state);
void recomp_lua_dafa0_stack_checkpoint(uint32_t stage, uint32_t stack,
                                       uint32_t value);
void recomp_lua_lex_trace(uint32_t xbox_va);
void recomp_dump_frontend_post_lua_state(void);
void recomp_lua_error_trace(uint32_t xbox_va);
void recomp_lua_concat_trace(uint32_t state, uint32_t total,
                             uint32_t last, uint32_t top,
                             uint32_t left, uint32_t right);
void recomp_lua_vm_register_trace(uint32_t tag, uint32_t stack,
                                  uint32_t state, uint32_t instruction,
                                  uint32_t destination);
void recomp_math_table_trace(uint32_t tag, uint32_t stack, uint32_t frame,
                             uint32_t output, uint32_t slot,
                             uint32_t counter, uint32_t source,
                             uint32_t cursor);
void recomp_asset_lookup_trace(uint32_t tag, uint32_t stack,
                               uint32_t object, uint32_t owner,
                               uint32_t table, uint32_t search,
                               uint32_t key);
void recomp_lua_gc_trace(uint32_t tag, uint32_t stack,
                         uint32_t state, uint32_t global,
                         uint32_t main_thread, uint32_t gc_state);
void recomp_pose_buffer_trace(uint32_t stage, uint32_t stack,
                              uint32_t object, uint32_t value);
void recomp_global_hash_trace(uint32_t stage, uint32_t stack,
                              uint32_t object, uint32_t key);
void recomp_global_list_validate(uint32_t stage, uint32_t stack);
void recomp_event_stack_trace(uint32_t xbox_va);
void recomp_script_use_checkpoint(uint32_t xbox_va);
void recomp_stream_update_trace(void);
void recomp_disk_error_trace(void);
void recomp_disc_cache_trace(uint32_t xbox_va);
void recomp_stream_header_trace(uint32_t phase, uint32_t anchor);
void recomp_d3d_set_stream_source(uint32_t stream, uint32_t vertex_buffer,
                                  uint32_t stride);
void recomp_brush3d_checkpoint(uint32_t stage, uint32_t pointer);
void recomp_brush2d_menu_checkpoint(void);
void recomp_movie_checkpoint(uint32_t stage, uint32_t object,
                             uint32_t path_address, uint32_t value0,
                             uint32_t value1);
void recomp_omni_light_trace(uint32_t index, uint32_t light,
                             uint32_t color);
void recomp_d3d_set_vertex_shader(uint32_t shader);
void recomp_d3d_select_vertex_shader_direct(uint32_t format,
                                             uint32_t start);
void recomp_d3d_constant_checkpoint(uint32_t constant, uint32_t source,
                                    uint32_t dword_count);
void recomp_humvee_render_item_checkpoint(uint32_t item);
void recomp_xact_stream_checkpoint(uint32_t object, uint32_t descriptor,
                                    uint32_t format);
void recomp_xact_list_checkpoint(uint32_t stage, uint32_t manager,
                                  uint32_t object);
void recomp_xact_cue_lifetime_checkpoint(uint32_t stage, uint32_t object, uint32_t result);
void recomp_xact_cue_checkpoint(uint32_t stage, uint32_t manager,
                                uint32_t cue, uint32_t result);
void recomp_xact_alloc_checkpoint(uint32_t stage, uint32_t owner,
                                  uint32_t object, uint32_t result);
void recomp_xact_setup_checkpoint(uint32_t stage, uint32_t manager,
                                  uint32_t cue_name, uint32_t value0,
                                  uint32_t value1);
void recomp_xact_play_checkpoint(uint32_t stage, uint32_t cue,
                                 uint32_t config, uint32_t result);
void recomp_xact_properties_checkpoint(uint32_t wrapper, uint32_t properties,
                                       uint32_t result);
void recomp_xact_managed_checkpoint(uint32_t stage, uint32_t handle,
                                    uint32_t cue_name, uint32_t value);
void recomp_xact_managed_update_checkpoint(uint32_t cue);
int recomp_xact_low_level_handle_exists(uint32_t handle);
void recomp_music_checkpoint(uint32_t stage, uint32_t state, uint32_t value);

void recomp_xact_bank_checkpoint(uint32_t stage, uint32_t manager);
void recomp_xact_voice_start_checkpoint(uint32_t stage, uint32_t voice,
                                         uint32_t value0, uint32_t value1);
int recomp_xact_stream_has_unsubmitted_read(uint32_t stream);
void recomp_xact_sound_update_checkpoint(uint32_t stage, uint32_t sound,
                                          uint32_t event, uint32_t value);
void recomp_dsound_object_checkpoint(uint32_t stage, uint32_t wrapper,
                                      uint32_t inner);
void recomp_dsound_stream_checkpoint(uint32_t stage, uint32_t inner,
                                      uint32_t packet, uint32_t value);
void recomp_xmv_audio_selector_checkpoint(uint32_t decoder, uint32_t index,
                                           uint32_t state_a, uint32_t state_b,
                                           uint32_t phase);
uint32_t recomp_movie_audio_enabled(uint32_t requested);
void recomp_effect_matrix_checkpoint(uint32_t stage, uint32_t saved_esp,
                                     uint32_t actual_esp,
                                     uint32_t saved_edi,
                                     uint32_t actual_edi);
void recomp_pbl_file_checkpoint(uint32_t stage, uint32_t object,
                                uint32_t value);
uint32_t recomp_traffic_random_path_target(uint32_t manager,
                                           uint32_t path_record,
                                           uint32_t target);
void recomp_traffic_spawn_checkpoint(uint32_t stage, uint32_t manager,
                                     uint32_t path_record,
                                     uint32_t spawn_type,
                                     uint32_t point_index,
                                     uint32_t matrix, uint32_t result);
void recomp_traffic_update_checkpoint(uint32_t manager, float distance_sq,
                                      float camera_x, float camera_y,
                                      float camera_z, float previous_x,
                                      float previous_y, float previous_z);
void recomp_traffic_detach_checkpoint(uint32_t stage, uint32_t path_record,
                                      uint32_t actor, uint32_t destroyed);
void recomp_traffic_release_checkpoint(uint32_t site, uint32_t ai, uint32_t destroyed);
void recomp_traffic_release_checkpoint(uint32_t site, uint32_t ai, uint32_t destroyed);
/* Xbox-rate timestamp counter used by lifted RDTSC instructions. */
uint64_t xbox_ReadTimeStampCounter(void);


/* Current translated function, retained in all configurations for bounded diagnostics. */
extern volatile uint32_t g_recomp_current_func;
extern volatile uint32_t g_recomp_entry_trace_enabled;
extern volatile uint32_t g_recomp_notification_owner_watch_enabled;
extern volatile uint32_t g_recomp_watchdog_heartbeat_enabled;
extern volatile uint32_t g_recomp_watchdog_heartbeat;
extern volatile uint32_t g_recomp_irq_entry_safepoint_enabled;
extern volatile int32_t g_kernel_pending_hardware_interrupts;
void xbox_kernel_service_hardware_interrupts(void);
extern volatile int32_t g_kernel_pending_guest_timers;
void xbox_kernel_service_guest_timers(void);
extern volatile int32_t g_kernel_pending_guest_dpcs;
void xbox_kernel_service_guest_dpcs(void);
extern volatile uint32_t g_recomp_target_call_trace_enabled;
extern volatile uint32_t g_recomp_recent_funcs[64];
extern volatile uint32_t g_recomp_recent_func_idx;
extern volatile uint32_t g_recomp_recent_game_funcs[256];
extern volatile uint32_t g_recomp_recent_game_func_idx;
void recomp_stack_collapse_trace(uint32_t xbox_va);
void recomp_guest_watch_trace(uint32_t xbox_va);
void recomp_xmv_func_trace(uint32_t xbox_va);
void recomp_xmv_idct_checkpoint(uint32_t stage, uint32_t output,
                                uint32_t coefficients, uint32_t block_mask);
void recomp_xmv_mc_checkpoint(uint32_t stage, uint32_t source,
                              uint32_t source_stride, uint32_t destination,
                              uint32_t destination_stride, uint32_t horizontal,
                              uint32_t vertical, uint32_t residual);
void recomp_xmv_predictor_checkpoint(uint32_t stage, uint32_t routine,
                                     uint32_t source, uint32_t source_stride,
                                     uint32_t destination,
                                     uint32_t destination_stride,
                                     uint32_t horizontal, uint32_t vertical,
                                     uint32_t auxiliary, uint32_t residual);
uint32_t recomp_dsound_clock_value(uint32_t device, uint32_t guest_clock);
void recomp_xmv_macroblock_checkpoint(uint32_t stage);
void recomp_xmv_macroblock_args_trace(void);
void recomp_xmv_frame_decode_checkpoint(uint32_t stage, uint32_t decoder);
void recomp_xmv_mmx_checkpoint(uint32_t stage);
void recomp_xmv_interblock_checkpoint(uint32_t stage);
void recomp_xmv_block_checkpoint(uint32_t stage);
void recomp_xmv_block_final_trace(uint32_t eax, uint32_t ebx, uint32_t ecx,
                                  uint32_t edx, uint32_t esi, uint32_t edi,
                                  uint32_t ebp);
void recomp_xmv_block_residual_trace(uint32_t site, uint32_t address,
                                     uint32_t old_value, uint32_t new_value,
                                     uint32_t ebp);
void recomp_xmv_block_sign_trace(uint32_t result, uint32_t ebp);
void recomp_xmv_intra_checkpoint(uint32_t stage);
void recomp_xmv_coeff_checkpoint(uint32_t stage, uint32_t bitreader,
                                 uint32_t quantizer, uint32_t vlc_table,
                                 uint32_t scan_table, uint32_t coefficients,
                                 uint32_t state1, uint32_t state2,
                                 uint32_t result);
void recomp_xmv_transform_checkpoint(uint32_t stage, uint32_t routine,
                                     uint32_t output, uint32_t coefficients,
                                     uint32_t block_index);
void recomp_target_call_trace(uint32_t xbox_va, const char *caller,
                              uint32_t line, uint32_t stack,
                              uint32_t frame, uint32_t saved_stack);
void recomp_stack_owner_step_trace(uint32_t label);
void recomp_vehicle_wheel_actor_checkpoint(uint32_t actor, uint32_t quality);
void recomp_teardown_call_checkpoint(uint32_t site, uint32_t expected_esp,
                                      uint32_t expected_esi);
void recomp_teardown_item_checkpoint(uint32_t site, uint32_t actor);
void recomp_icall_stack_mismatch_trace(uint32_t xbox_va, const char *caller,
                                       uint32_t line, uint32_t expected_stack,
                                       uint32_t actual_stack, uint32_t frame);
extern volatile uint32_t g_recomp_recent_asset_requests[512];
extern volatile uint32_t g_recomp_recent_asset_request_idx;

#define RECOMP_TRACE_RECENT(xbox_va) do { \
    uint32_t _trace_va = (uint32_t)(xbox_va); \
    if (g_recomp_irq_entry_safepoint_enabled && \
        g_kernel_pending_hardware_interrupts != 0) \
        xbox_kernel_service_hardware_interrupts(); \
    if (g_kernel_pending_guest_timers != 0) \
        xbox_kernel_service_guest_timers(); \
    if (g_kernel_pending_guest_dpcs != 0) \
        xbox_kernel_service_guest_dpcs(); \
    g_recomp_current_func = _trace_va; \
    if (g_recomp_notification_owner_watch_enabled) \
        recomp_notification_owner_watch_checkpoint(_trace_va); \
    if (g_recomp_collision_agent_watch_enabled) \
        recomp_collision_agent_watch_checkpoint(_trace_va); \
    if (g_recomp_target_manager_watch_enabled) \
        recomp_target_manager_watch_checkpoint(_trace_va); \
    if (g_recomp_particle_list_watch_enabled) \
        recomp_particle_list_watch_checkpoint(_trace_va); \
    if (g_recomp_watchdog_heartbeat_enabled) \
        ++g_recomp_watchdog_heartbeat; \
    if (_trace_va == 0x00091D90u) \
        recomp_player_vehicle_exit_checkpoint(0u, g_ecx); \
    if (_trace_va == 0x0008C810u) \
        recomp_player_vehicle_exit_checkpoint(1u, g_ecx); \
    if (_trace_va == 0x00045C50u) \
        recomp_vehicle_wheel_actor_checkpoint(g_ecx, MEM32(g_esp + 4u)); \
    if (_trace_va == 0x002157A0u) \
        recomp_humvee_render_item_checkpoint(MEM32(g_esp + 4u)); \
    if (_trace_va == 0x001DE230u) \
        recomp_lua_checkcode_checkpoint(); \
    if (_trace_va == 0x001DDE70u) \
        recomp_lua_symbexec_checkpoint(); \
    if (_trace_va == 0x001DE760u) \
        recomp_lua_typeerror_checkpoint(); \
    if (_trace_va == 0x001DE4B0u || _trace_va == 0x001DE930u) \
        recomp_lua_error_trace(_trace_va); \
    if (g_recomp_entry_trace_enabled) { \
        uint32_t _trace_idx = g_recomp_recent_func_idx++; \
        g_recomp_recent_funcs[_trace_idx & 63u] = _trace_va; \
        if (_trace_va < 0x00280000u && _trace_va != 0x002092A0u && \
            _trace_va != 0x00209B80u) { \
            uint32_t _game_idx = g_recomp_recent_game_func_idx; \
            if (_game_idx == 0u || \
                g_recomp_recent_game_funcs[(_game_idx - 1u) & 255u] != _trace_va) { \
                g_recomp_recent_game_funcs[_game_idx & 255u] = _trace_va; \
                g_recomp_recent_game_func_idx = _game_idx + 1u; \
            } \
        } \
        if (g_esp < 0x00010000u) \
            recomp_stack_collapse_trace(_trace_va); \
        recomp_guest_watch_trace(_trace_va); \
        if (_trace_va >= 0x00255620u && _trace_va < 0x0027D5B4u) \
            recomp_xmv_func_trace(_trace_va); \
        if (_trace_va >= 0x001D0000u && _trace_va < 0x001F0000u) \
            recomp_lua_lex_trace(_trace_va); \
        if (_trace_va == 0x00186E90u || _trace_va == 0x00187020u || \
            _trace_va == 0x00187BA0u || \
            _trace_va == 0x00187430u || _trace_va == 0x001874B0u) \
            recomp_event_stack_trace(_trace_va); \
        if (_trace_va == 0x00116BA0u || _trace_va == 0x00116D10u || \
            _trace_va == 0x00119FE0u || _trace_va == 0x0011A0E0u || \
            _trace_va == 0x0011AAD0u) { \
            recomp_event_stack_trace(_trace_va); \
            recomp_script_use_checkpoint(_trace_va); \
        } \
        if (_trace_va == 0x00222A10u) \
            recomp_stream_update_trace(); \
        if (_trace_va == 0x00209760u) \
            recomp_disk_error_trace(); \
        if (_trace_va == 0x00209100u || _trace_va == 0x00209200u || \
            _trace_va == 0x00209810u || _trace_va == 0x002092A0u) \
            recomp_disc_cache_trace(_trace_va); \
        if (_trace_va == 0x002181C0u) { \
            uint32_t _asset_idx = g_recomp_recent_asset_request_idx++; \
            g_recomp_recent_asset_requests[_asset_idx & 511u] = g_ecx; \
        } \
    } \
} while (0)
/* Optional function-entry trace used to diagnose translated call cycles. */
#if defined(ENABLE_RECOMP_FUNC_TRACE)
void recomp_func_trace(uint32_t xbox_va);
#define RECOMP_TRACE_FUNC(xbox_va) do { \
    RECOMP_TRACE_RECENT(xbox_va); \
    recomp_func_trace((uint32_t)(xbox_va)); \
} while (0)
#else
#define RECOMP_TRACE_FUNC(xbox_va) \
    RECOMP_TRACE_RECENT(xbox_va)
#endif

/* ================================================================
 * Memory access helpers
 * ================================================================ */

/**
 * Translate an Xbox VA to an actual pointer.
 * Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 * in the recompiled code can overflow. Without the mask, a 64-bit
 * uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 * our mapping and causing access violations.
 */
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
#define MEM64(addr)  (*(volatile uint64_t *)XBOX_PTR(addr))

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))

/** Float/double memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/* Full 128-bit XMM helpers. Scalar SSE instructions use lane zero through
 * the xmmN aliases below; packed instructions use the xmmNv aliases. */
static __forceinline void recomp_xmm_load(float dst[4], uint32_t addr) {
    memcpy(dst, (const void *)XBOX_PTR(addr), 16);
}
static __forceinline void recomp_xmm_store(uint32_t addr, const float src[4]) {
    memcpy((void *)XBOX_PTR(addr), src, 16);
}
static __forceinline void recomp_xmm_copy(float dst[4], const float src[4]) {
    memcpy(dst, src, 16);
}
static __forceinline void recomp_xmm_zero(float dst[4]) {
    memset(dst, 0, 16);
}
static __forceinline void recomp_xmm_loadss(float dst[4], uint32_t addr) {
    memcpy(&dst[0], (const void *)XBOX_PTR(addr), 4);
    dst[1] = dst[2] = dst[3] = 0.0f;
}
static __forceinline void recomp_xmm_loadsd(float dst[4], uint32_t addr) {
    memcpy(&dst[0], (const void *)XBOX_PTR(addr), 8);
    dst[2] = dst[3] = 0.0f;
}
static __forceinline void recomp_xmm_copysd(float dst[4], const float src[4]) {
    memcpy(&dst[0], &src[0], 8);
}
static __forceinline void recomp_xmm_storesd(uint32_t addr, const float src[4]) {
    memcpy((void *)XBOX_PTR(addr), &src[0], 8);
}
static __forceinline void recomp_xmm_movlps_load(float dst[4], uint32_t addr) {
    memcpy(&dst[0], (const void *)XBOX_PTR(addr), 8);
}
static __forceinline void recomp_xmm_movlps_store(uint32_t addr, const float src[4]) {
    memcpy((void *)XBOX_PTR(addr), &src[0], 8);
}
static __forceinline void recomp_xmm_movhps_load(float dst[4], uint32_t addr) {
    memcpy(&dst[2], (const void *)XBOX_PTR(addr), 8);
}
static __forceinline void recomp_xmm_movhps_store(uint32_t addr, const float src[4]) {
    memcpy((void *)XBOX_PTR(addr), &src[2], 8);
}
static __forceinline void recomp_xmm_movlhps(float dst[4], const float src[4]) {
    float s0 = src[0], s1 = src[1]; dst[2] = s0; dst[3] = s1;
}
static __forceinline void recomp_xmm_movhlps(float dst[4], const float src[4]) {
    float s2 = src[2], s3 = src[3]; dst[0] = s2; dst[1] = s3;
}
static __forceinline void recomp_xmm_set_u32(float dst[4], uint32_t value) {
    memcpy(&dst[0], &value, 4); dst[1] = dst[2] = dst[3] = 0.0f;
}
static __forceinline uint32_t recomp_xmm_get_u32(const float src[4]) {
    uint32_t value; memcpy(&value, &src[0], 4); return value;
}
static __forceinline void recomp_xmm_movq_copy(float dst[4], const float src[4]) {
    memcpy(&dst[0], &src[0], 8); dst[2] = dst[3] = 0.0f;
}
static __forceinline void recomp_xmm_movq_load(float dst[4], uint32_t addr) {
    recomp_xmm_loadsd(dst, addr);
}
static __forceinline float recomp_xmm_min_sse(float a, float b) {
    return (isnan(a) || isnan(b) || a == b) ? b : (a < b ? a : b);
}
static __forceinline float recomp_xmm_max_sse(float a, float b) {
    return (isnan(a) || isnan(b) || a == b) ? b : (a > b ? a : b);
}
#define RECOMP_XMM_BINARY_RR(dst, src, op) do { \
    for (unsigned _xmm_i = 0; _xmm_i < 4; ++_xmm_i) \
        (dst)[_xmm_i] = (dst)[_xmm_i] op (src)[_xmm_i]; \
} while (0)
#define RECOMP_XMM_BINARY_RM(dst, addr, op) do { \
    float _xmm_src[4]; recomp_xmm_load(_xmm_src, (uint32_t)(addr)); \
    RECOMP_XMM_BINARY_RR((dst), _xmm_src, op); \
} while (0)
static __forceinline void recomp_xmm_minps(float dst[4], const float src[4]) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = recomp_xmm_min_sse(dst[i], src[i]);
}
static __forceinline void recomp_xmm_maxps(float dst[4], const float src[4]) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = recomp_xmm_max_sse(dst[i], src[i]);
}
static __forceinline void recomp_xmm_minps_mem(float dst[4], uint32_t addr) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_minps(dst, src);
}
static __forceinline void recomp_xmm_maxps_mem(float dst[4], uint32_t addr) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_maxps(dst, src);
}
static __forceinline uint32_t recomp_xmm_lane_bits(const float src[4], unsigned lane) {
    uint32_t value; memcpy(&value, &src[lane], 4); return value;
}
static __forceinline void recomp_xmm_set_lane_bits(float dst[4], unsigned lane,
                                                   uint32_t value) {
    memcpy(&dst[lane], &value, 4);
}
static __forceinline void recomp_xmm_bitwise(float dst[4], const float src[4],
                                             unsigned operation) {
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t a = recomp_xmm_lane_bits(dst, i);
        uint32_t b = recomp_xmm_lane_bits(src, i);
        uint32_t value = operation == 0u ? (a ^ b) :
                         operation == 1u ? (a & b) :
                         operation == 2u ? (a | b) : ((~a) & b);
        recomp_xmm_set_lane_bits(dst, i, value);
    }
}
static __forceinline void recomp_xmm_bitwise_mem(float dst[4], uint32_t addr,
                                                 unsigned operation) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_bitwise(dst, src, operation);
}
static __forceinline void recomp_xmm_shufps(float dst[4], const float src[4],
                                            uint8_t control) {
    float a[4], b[4], out[4];
    recomp_xmm_copy(a, dst); recomp_xmm_copy(b, src);
    out[0] = a[(control >> 0) & 3u]; out[1] = a[(control >> 2) & 3u];
    out[2] = b[(control >> 4) & 3u]; out[3] = b[(control >> 6) & 3u];
    recomp_xmm_copy(dst, out);
}
static __forceinline void recomp_xmm_shufps_mem(float dst[4], uint32_t addr,
                                                uint8_t control) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_shufps(dst, src, control);
}
static __forceinline void recomp_xmm_unpcklps(float dst[4], const float src[4]) {
    float a0 = dst[0], a1 = dst[1], b0 = src[0], b1 = src[1];
    dst[0] = a0; dst[1] = b0; dst[2] = a1; dst[3] = b1;
}
static __forceinline void recomp_xmm_unpckhps(float dst[4], const float src[4]) {
    float a2 = dst[2], a3 = dst[3], b2 = src[2], b3 = src[3];
    dst[0] = a2; dst[1] = b2; dst[2] = a3; dst[3] = b3;
}
static __forceinline void recomp_xmm_unpcklps_mem(float dst[4], uint32_t addr) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_unpcklps(dst, src);
}
static __forceinline void recomp_xmm_unpckhps_mem(float dst[4], uint32_t addr) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_unpckhps(dst, src);
}
static __forceinline void recomp_xmm_unary_ps(float dst[4], const float src[4],
                                              unsigned operation) {
    float input[4]; recomp_xmm_copy(input, src);
    for (unsigned i = 0; i < 4; ++i) {
        dst[i] = operation == 0u ? sqrtf(input[i]) :
                 operation == 1u ? 1.0f / sqrtf(input[i]) : 1.0f / input[i];
    }
}
static __forceinline void recomp_xmm_unary_ps_mem(float dst[4], uint32_t addr,
                                                  unsigned operation) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_unary_ps(dst, src, operation);
}
static __forceinline void recomp_xmm_cmp_ps(float dst[4], const float src[4],
                                            unsigned predicate) {
    float a[4], b[4]; recomp_xmm_copy(a, dst); recomp_xmm_copy(b, src);
    for (unsigned i = 0; i < 4; ++i) {
        int unordered = isnan(a[i]) || isnan(b[i]);
        int result = predicate == 0u ? (unordered || a[i] != b[i]) :
                     predicate == 1u ? (!unordered && a[i] == b[i]) :
                     predicate == 2u ? (!unordered && a[i] < b[i]) :
                                       (!unordered && a[i] <= b[i]);
        recomp_xmm_set_lane_bits(dst, i, result ? 0xFFFFFFFFu : 0u);
    }
}
static __forceinline void recomp_xmm_cmp_ps_mem(float dst[4], uint32_t addr,
                                                unsigned predicate) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_cmp_ps(dst, src, predicate);
}
static __forceinline uint32_t recomp_xmm_movmskps(const float src[4]) {
    uint32_t mask = 0;
    for (unsigned i = 0; i < 4; ++i)
        mask |= ((recomp_xmm_lane_bits(src, i) >> 31) & 1u) << i;
    return mask;
}
/* Keep ordinary RAM copies fast, but perform MMIO copies through volatile
 * scalar accesses so the fault bridge can emulate each hardware register. */
static __forceinline void XBOX_MEMCPY(uint32_t destination, uint32_t source,
                                      uint32_t length) {
    uint32_t offset = 0;
    if (destination < 0xFD000000u && source < 0xFD000000u) {
        memcpy((void *)XBOX_PTR(destination), (const void *)XBOX_PTR(source),
               length);
        return;
    }
    while (length - offset >= 4u &&
           (((destination + offset) | (source + offset)) & 3u) == 0u) {
        MEM32(destination + offset) = MEM32(source + offset);
        offset += 4u;
    }
    while (offset < length) {
        MEM8(destination + offset) = MEM8(source + offset);
        ++offset;
    }
}
static __forceinline void XBOX_REP_MOVS(uint32_t destination, uint32_t source,
                                       uint32_t count, uint32_t width) {
    uint64_t length = (uint64_t)count * width;
    if (count == 0u) return;
    if ((uint64_t)destination + length <= 0xFD000000ull &&
        (uint64_t)source + length <= 0xFD000000ull) {
        if ((uint64_t)destination + length <= source ||
            (uint64_t)source + length <= destination) {
            memcpy((void *)XBOX_PTR(destination), (const void *)XBOX_PTR(source),
                   (size_t)length);
            return;
        }
        if (destination <= source) {
            memmove((void *)XBOX_PTR(destination), (const void *)XBOX_PTR(source),
                    (size_t)length);
            return;
        }
    }
    /* Volatile guest accesses preserve element width/order for overlap and
     * hardware registers, including unaligned word/dword instructions. */
    while (count-- != 0u) {
        if (width == 4u) MEM32(destination) = MEM32(source);
        else if (width == 2u) MEM16(destination) = MEM16(source);
        else MEM8(destination) = MEM8(source);
        destination += width;
        source += width;
    }
}
/** Apply the guest x87 rounding-control field for FIST/FISTP. */
static inline double x87_round_integral(double value) {
    double rounded;
    switch ((g_x87_control_word >> 10) & 3u) {
    case 1u: rounded = floor(value); break;
    case 2u: rounded = ceil(value); break;
    case 3u: rounded = trunc(value); break;
    default: rounded = nearbyint(value); break;
    }
    if (rounded != value)
        g_x87_status_word |= 0x20u;
    return rounded;
}
static inline int16_t X87_FIST16(double value) {
    double rounded = x87_round_integral(value);
    return (!isfinite(rounded) || rounded < -32768.0 || rounded >= 32768.0)
        ? INT16_MIN : (int16_t)rounded;
}
static inline int32_t X87_FIST32(double value) {
    double rounded = x87_round_integral(value);
    return (!isfinite(rounded) || rounded < -2147483648.0 || rounded >= 2147483648.0)
        ? INT32_MIN : (int32_t)rounded;
}
static inline int64_t X87_FIST64(double value) {
    double rounded = x87_round_integral(value);
    return (!isfinite(rounded) || rounded < -9223372036854775808.0 ||
            rounded >= 9223372036854775808.0)
        ? INT64_MIN : (int64_t)rounded;
}

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
#define CMP_L(a, b)   ((int32_t)(a) <  (int32_t)(b))     /* less (SF!=OF) */
#define CMP_GE(a, b)  ((int32_t)(a) >= (int32_t)(b))     /* greater or equal */
#define CMP_LE(a, b)  ((int32_t)(a) <= (int32_t)(b))     /* less or equal */
#define CMP_G(a, b)   ((int32_t)(a) >  (int32_t)(b))     /* greater */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
static inline int EVEN_PARITY8(uint8_t value) {
    value ^= (uint8_t)(value >> 4);
    return (int)((0x9669u >> (value & 0xFu)) & 1u);
}
#define TEST_S(a, b)  (sizeof(a) == 1 ? \
    ((((uint8_t)(a) & (uint8_t)(b)) & 0x80u) != 0) : \
    (sizeof(a) == 2 ? \
        ((((uint16_t)(a) & (uint16_t)(b)) & 0x8000u) != 0) : \
        ((((uint32_t)(a) & (uint32_t)(b)) & 0x80000000u) != 0))) /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return (val << n) | (val >> (32 - n));
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return (val >> n) | (val << (32 - n));
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ================================================================
 * Byte/word register access
 *
 * These macros extract or set partial registers, matching x86
 * behavior where writing AL doesn't affect bits 8-31 of EAX.
 * ================================================================ */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte, preserving upper 24 bits. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word, preserving other bits. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word, preserving upper 16 bits. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ================================================================
 * Stack simulation
 *
 * For push/pop heavy prologues in the generated code.
 * ================================================================ */

/**
 * Push a 32-bit value onto the simulated stack.
 * Evaluates val BEFORE decrementing sp, matching x86 semantics
 * where push [esp+N] reads the operand before adjusting ESP.
 */
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    MEM32(sp) = _pv; \
} while(0)

/** Pop a 32-bit value from the simulated stack. */
#define POP32(sp, dst) do { \
    (dst) = MEM32(sp); \
    (sp) += 4; \
} while(0)

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

#define RECOMP_TARGET_CALL_TRACE(va, saved_stack) do { \
    uint32_t _target_trace_va = (uint32_t)(va); \
    if (!g_recomp_target_call_trace_enabled) break; \
    if (_target_trace_va == 0x0006C420u || _target_trace_va == 0x00146730u || \
        _target_trace_va == 0x00190B70u || \
        _target_trace_va == 0x00043A60u || \
        _target_trace_va == 0x001C8990u || \
        _target_trace_va == 0x001874B0u || _target_trace_va == 0x0020DAF0u || \
        _target_trace_va == 0x002376FCu || \
        strcmp(__FUNCTION__, "sub_001940D0") == 0) \
        recomp_target_call_trace(_target_trace_va, __FUNCTION__, __LINE__, \
                                 g_esp, g_seh_ebp, \
                                 (uint32_t)(saved_stack)); \
} while (0)

/**
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 a dummy return address before this macro.
 * If not found, pops the dummy return address to keep the stack balanced.
 *
 * Targets in [0x00400000, 0xFE000000) are rejected before lookup.
 * Kernel thunks at 0xFE000000 and above remain eligible for dispatch.
 * A rejected or unresolved target consumes the return address and returns zero.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    const uint32_t _callee_saved_ebx = g_ebx; \
    const uint32_t _callee_saved_esi = g_esi; \
    const uint32_t _callee_saved_edi = g_edi; \
    RECOMP_TARGET_CALL_TRACE(_va, 0u); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { \
        _fn(); \
        g_ebx = _callee_saved_ebx; \
        g_esi = _callee_saved_esi; \
        g_edi = _callee_saved_edi; \
    } \
    else { g_esp += 4; eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE - Stack-safe indirect call.
 *
 * Restores g_esp to saved_esp (pre-argument value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 * Use this when the caller pushes arguments that the callee would
 * normally clean up (stdcall convention).
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    const uint32_t _callee_saved_ebx = g_ebx; \
    const uint32_t _callee_saved_esi = g_esi; \
    const uint32_t _callee_saved_edi = g_edi; \
    RECOMP_TARGET_CALL_TRACE(_va, (uint32_t)(saved_esp)); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { \
        _fn(); \
        /* EBX, ESI, and EDI are callee-saved in the Xbox x86 ABI.  Keep the
         * caller contract even when a recovered/generated callee has an
         * incomplete epilogue or exits through a translated tail path. */ \
        g_ebx = _callee_saved_ebx; \
        g_esi = _callee_saved_esi; \
        g_edi = _callee_saved_edi; \
        if (((uint32_t)(saved_esp) >= 0x00010000u && \
             g_esp < 0x00010000u) || \
            (strcmp(__FUNCTION__, "sub_00069550") == 0 && \
             g_esp != (uint32_t)(saved_esp))) \
            recomp_icall_stack_mismatch_trace( \
                _va, __FUNCTION__, __LINE__, (uint32_t)(saved_esp), \
                g_esp, g_seh_ebp); \
    } \
    else { g_esp = (saved_esp); eax = 0; } \
} while(0)

/**
 * RECOMP_ITAIL - Indirect tail call (jmp through function pointer).
 *
 * No return address is pushed - reuses the current frame's return addr.
 * Used for tail-call optimization where the original code uses
 * jmp [reg] instead of call [reg].
 */
#define RECOMP_ITAIL(xbox_va) do { \
    RECOMP_TARGET_CALL_TRACE((uint32_t)(xbox_va), 0u); \
    recomp_func_t _fn = recomp_lookup_manual((uint32_t)(xbox_va)); \
    if (!_fn) _fn = recomp_lookup((uint32_t)(xbox_va)); \
    if (!_fn) _fn = recomp_lookup_kernel((uint32_t)(xbox_va)); \
    if (_fn) _fn(); \
} while(0)

/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 * ================================================================ */

#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define xmm0 g_xmm0[0]
#define xmm1 g_xmm1[0]
#define xmm2 g_xmm2[0]
#define xmm3 g_xmm3[0]
#define xmm4 g_xmm4[0]
#define xmm5 g_xmm5[0]
#define xmm6 g_xmm6[0]
#define xmm7 g_xmm7[0]
#define xmm0v g_xmm0
#define xmm1v g_xmm1
#define xmm2v g_xmm2
#define xmm3v g_xmm3
#define xmm4v g_xmm4
#define xmm5v g_xmm5
#define xmm6v g_xmm6
#define xmm7v g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7

#include "xmv_fast_helpers.h"
static __forceinline uint64_t recomp_mmx_pcmpgtw(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int16_t a = (int16_t)(uint16_t)(left >> (i * 16));
        int16_t b = (int16_t)(uint16_t)(right >> (i * 16));
        result |= (uint64_t)(a > b ? 0xFFFFu : 0u) << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pcmpgtd(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int32_t a = (int32_t)(uint32_t)(left >> (i * 32));
        int32_t b = (int32_t)(uint32_t)(right >> (i * 32));
        result |= (uint64_t)(a > b ? UINT32_MAX : 0u) << (i * 32);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pcmpgtb(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        int8_t a = (int8_t)(uint8_t)(left >> (i * 8));
        int8_t b = (int8_t)(uint8_t)(right >> (i * 8));
        result |= (uint64_t)(a > b ? 0xFFu : 0u) << (i * 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pcmpeqb(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= (uint64_t)(((uint8_t)(left >> (i * 8)) ==
                              (uint8_t)(right >> (i * 8))) ? 0xFFu : 0u)
                  << (i * 8);
    return result;
}
static __forceinline uint64_t recomp_mmx_pcmpeqw(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= (uint64_t)(((uint16_t)(left >> (i * 16)) ==
                              (uint16_t)(right >> (i * 16))) ? 0xFFFFu : 0u)
                  << (i * 16);
    return result;
}
static __forceinline uint64_t recomp_mmx_paddb(uint64_t left,
                                                uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= (uint64_t)(uint8_t)((uint8_t)(left >> (i * 8)) +
                                      (uint8_t)(right >> (i * 8))) << (i * 8);
    return result;
}
static __forceinline uint64_t recomp_mmx_psubb(uint64_t left,
                                                uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= (uint64_t)(uint8_t)((uint8_t)(left >> (i * 8)) -
                                      (uint8_t)(right >> (i * 8))) << (i * 8);
    return result;
}
static __forceinline uint64_t recomp_mmx_psubd(uint64_t left,
                                                uint64_t right) {
    uint32_t lo = (uint32_t)left - (uint32_t)right;
    uint32_t hi = (uint32_t)(left >> 32) - (uint32_t)(right >> 32);
    return lo | ((uint64_t)hi << 32);
}
static __forceinline uint64_t recomp_mmx_pmullw(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t value = (uint16_t)((int16_t)(left >> (i * 16)) *
                                    (int16_t)(right >> (i * 16)));
        result |= (uint64_t)value << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pavgb(uint64_t left,
                                                uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned a = (uint8_t)(left >> (i * 8));
        unsigned b = (uint8_t)(right >> (i * 8));
        result |= (uint64_t)((a + b + 1u) >> 1) << (i * 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_punpckldq(uint64_t left,
                                                    uint64_t right) {
    return (uint32_t)left | ((uint64_t)(uint32_t)right << 32);
}
static __forceinline uint64_t recomp_mmx_punpckhdq(uint64_t left,
                                                    uint64_t right) {
    return (uint32_t)(left >> 32) | ((uint64_t)(uint32_t)(right >> 32) << 32);
}
static __forceinline uint64_t recomp_mmx_paddw(uint64_t left,
                                               uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t value = (uint16_t)((uint16_t)(left >> (i * 16)) +
                                    (uint16_t)(right >> (i * 16)));
        result |= (uint64_t)value << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_psubw(uint64_t left,
                                               uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t value = (uint16_t)((uint16_t)(left >> (i * 16)) -
                                    (uint16_t)(right >> (i * 16)));
        result |= (uint64_t)value << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_paddd(uint64_t left,
                                               uint64_t right) {
    uint64_t lo = (uint32_t)left + (uint32_t)right;
    uint64_t hi = (uint32_t)(left >> 32) + (uint32_t)(right >> 32);
    return (uint32_t)lo | ((uint64_t)(uint32_t)hi << 32);
}
static __forceinline uint64_t recomp_mmx_pmaddwd(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned pair = 0; pair < 2; ++pair) {
        unsigned lane = pair * 2;
        int32_t a0 = (int16_t)(left >> (lane * 16));
        int32_t a1 = (int16_t)(left >> ((lane + 1) * 16));
        int32_t b0 = (int16_t)(right >> (lane * 16));
        int32_t b1 = (int16_t)(right >> ((lane + 1) * 16));
        uint32_t value = (uint32_t)(a0 * b0 + a1 * b1);
        result |= (uint64_t)value << (pair * 32);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_punpcklbw(uint64_t left,
                                                    uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        result |= ((left >> (i * 8)) & 0xFFu) << (i * 16);
        result |= ((right >> (i * 8)) & 0xFFu) << (i * 16 + 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_punpckhbw(uint64_t left,
                                                    uint64_t right) {
    return recomp_mmx_punpcklbw(left >> 32, right >> 32);
}
static __forceinline uint64_t recomp_mmx_punpcklwd(uint64_t left,
                                                    uint64_t right) {
    return (left & 0xFFFFu) | ((right & 0xFFFFu) << 16) |
           (((left >> 16) & 0xFFFFu) << 32) |
           (((right >> 16) & 0xFFFFu) << 48);
}
static __forceinline uint64_t recomp_mmx_punpckhwd(uint64_t left,
                                                    uint64_t right) {
    return recomp_mmx_punpcklwd(left >> 32, right >> 32);
}
static __forceinline int32_t recomp_mmx_clamp_s16(int32_t value) {
    return value < -32768 ? -32768 : (value > 32767 ? 32767 : value);
}
static __forceinline int32_t recomp_mmx_clamp_s8(int32_t value) {
    return value < -128 ? -128 : (value > 127 ? 127 : value);
}
static __forceinline uint32_t recomp_mmx_clamp_u8(int32_t value) {
    return value < 0 ? 0u : (value > 255 ? 255u : (uint32_t)value);
}
static __forceinline uint64_t recomp_mmx_packssdw(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int32_t value = (int32_t)(left >> (i * 32));
        result |= (uint64_t)(uint16_t)recomp_mmx_clamp_s16(value) << (i * 16);
        value = (int32_t)(right >> (i * 32));
        result |= (uint64_t)(uint16_t)recomp_mmx_clamp_s16(value) << ((i + 2) * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_packsswb(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int32_t value = (int16_t)(left >> (i * 16));
        result |= (uint64_t)(uint8_t)recomp_mmx_clamp_s8(value) << (i * 8);
        value = (int16_t)(right >> (i * 16));
        result |= (uint64_t)(uint8_t)recomp_mmx_clamp_s8(value) << ((i + 4) * 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_packuswb(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int32_t value = (int16_t)(left >> (i * 16));
        result |= (uint64_t)recomp_mmx_clamp_u8(value) << (i * 8);
        value = (int16_t)(right >> (i * 16));
        result |= (uint64_t)recomp_mmx_clamp_u8(value) << ((i + 4) * 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_psllw(uint64_t value, uint64_t count) {
    uint64_t result = 0;
    if (count >= 16) return 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= (uint64_t)(uint16_t)((uint16_t)(value >> (i * 16)) << count) << (i * 16);
    return result;
}
static __forceinline uint64_t recomp_mmx_psrlw(uint64_t value, uint64_t count) {
    uint64_t result = 0;
    if (count >= 16) return 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= (uint64_t)(uint16_t)((uint16_t)(value >> (i * 16)) >> count) << (i * 16);
    return result;
}
static __forceinline uint64_t recomp_mmx_psraw(uint64_t value, uint64_t count) {
    uint64_t result = 0;
    if (count > 15) count = 15;
    for (unsigned i = 0; i < 4; ++i) {
        int16_t lane = (int16_t)(value >> (i * 16));
        result |= (uint64_t)(uint16_t)(lane >> count) << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pslld(uint64_t value, uint64_t count) {
    if (count >= 32) return 0;
    return (uint32_t)((uint32_t)value << count) |
           ((uint64_t)(uint32_t)((uint32_t)(value >> 32) << count) << 32);
}
static __forceinline uint64_t recomp_mmx_psrld(uint64_t value, uint64_t count) {
    if (count >= 32) return 0;
    return (uint32_t)((uint32_t)value >> count) |
           ((uint64_t)(uint32_t)((uint32_t)(value >> 32) >> count) << 32);
}
static __forceinline uint64_t recomp_mmx_psrad(uint64_t value, uint64_t count) {
    uint64_t result = 0;
    if (count > 31) count = 31;
    for (unsigned i = 0; i < 2; ++i) {
        int32_t lane = (int32_t)(value >> (i * 32));
        result |= (uint64_t)(uint32_t)(lane >> count) << (i * 32);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_psllq(uint64_t value, uint64_t count) {
    return count >= 64 ? 0 : value << count;
}
static __forceinline uint64_t recomp_mmx_psrlq(uint64_t value, uint64_t count) {
    return count >= 64 ? 0 : value >> count;
}
static __forceinline uint64_t recomp_mmx_pshufw(uint64_t value,
                                                uint8_t control) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= ((value >> (((control >> (i * 2)) & 3u) * 16)) & 0xFFFFu) << (i * 16);
    return result;
}
static __forceinline void recomp_xmm_cvtpi2ps(float dst[4], uint64_t src) {
    dst[0] = (float)(int32_t)(uint32_t)src;
    dst[1] = (float)(int32_t)(uint32_t)(src >> 32);
}
static __forceinline uint32_t recomp_xmm_float_to_i32(float value,
                                                       unsigned truncate) {
    float rounded;
    if (!isfinite(value)) return 0x80000000u;
    rounded = truncate ? truncf(value) : nearbyintf(value);
    if (rounded < -2147483648.0f || rounded >= 2147483648.0f)
        return 0x80000000u;
    return (uint32_t)(int32_t)rounded;
}
static __forceinline uint64_t recomp_xmm_cvtps2pi(const float src[4],
                                                   unsigned truncate) {
    return recomp_xmm_float_to_i32(src[0], truncate) |
           ((uint64_t)recomp_xmm_float_to_i32(src[1], truncate) << 32);
}
#define RECOMP_RDTSC() do { \
    uint64_t _recomp_tsc = xbox_ReadTimeStampCounter(); \
    eax = (uint32_t)_recomp_tsc; \
    edx = (uint32_t)(_recomp_tsc >> 32); \
} while (0)

/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

#include "ps2_upgrades.h"

#endif /* RECOMP_TYPES_H */
