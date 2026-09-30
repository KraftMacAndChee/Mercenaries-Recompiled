/* Device-independent channel mapping. Magnitudes remain analog until output.
 * Channels: 8 digital buttons, 8 pressure buttons, 8 signed stick directions.
 * No runtime allocation and no device merging until after each mapping. */
#ifndef MERCENARIES_CONTROLS_MAPPING_H
#define MERCENARIES_CONTROLS_MAPPING_H
#define CONTROL_CHANNELS 24
#define CONTROL_CONTEXTS 13
#define SCOPE_CONTEXT 4
static void controls_map_channels(const float raw[CONTROL_CHANNELS],
                                  const unsigned short map[CONTROL_CHANNELS],
                                  float out[CONTROL_CHANNELS]) {
    for (unsigned i=0; i<CONTROL_CHANNELS; ++i)
        out[i] = map[i] < CONTROL_CHANNELS ? raw[map[i]] : 0.0f;
}
/* Multiple actions may share a source. Only change the selected action. */
static void controls_assign(unsigned short map[CONTROL_CHANNELS], unsigned action,
                            unsigned short source) {
    map[action]=source;
}
static int controls_binding_context(int context) {
    /* Keep persisted section numbers stable. Merged modes resolve to their
     * owner for both input and prompts, never a second parallel map. */
    if(context==4 || context==9)return SCOPE_CONTEXT;
    if(context>=5 && context<=8)return 3;
    if(context==10)return 2;
    return context;
}
static int controls_context(unsigned joystick) {
    /* HQ roaming and scripted briefing prompts use the player's On Foot
     * bindings. Retail still decides which actions each briefing permits. */
    if(joystick==9 || joystick==13 || joystick==14)return 3;
    return joystick>=1 && joystick<=12 ? controls_binding_context((int)joystick-1) : -1;
}
static int controls_capture_allowed(int enabled,int gameplay,int focused,int overlay,int menu) {
    return enabled && gameplay && focused && !overlay && !menu;
}
#endif
