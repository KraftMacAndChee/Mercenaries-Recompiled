/* Last deliberate input wins. Polling a held input is not a new action. */
#ifndef XBOX_PROMPT_ACTIVITY_H
#define XBOX_PROMPT_ACTIVITY_H
#include <stdint.h>
#include <stdlib.h>
typedef struct PromptPadActivity {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t axes[4];
} PromptPadActivity;
static int prompt_pad_activity(PromptPadActivity *last, uint16_t buttons,
                               const uint8_t analog[8], const int16_t axes[4]) {
    int active=(buttons & ~last->buttons)!=0;
    last->buttons=buttons;
    for(unsigned i=0;i<8;++i) {
        /* Face buttons and triggers: reject noise and releases. */
        if(analog[i]>=48 && (last->analog[i]<48 || (int)analog[i]-(int)last->analog[i]>16))active=1;
        last->analog[i]=analog[i];
    }
    for(unsigned i=0;i<4;++i) {
        /* Compare to the last accepted position, allowing slow intentional
         * movement to accumulate without reacting to normal stick drift. */
        if(abs((int)axes[i])>8192 && abs((int)axes[i]-(int)last->axes[i])>1536) {
            active=1;last->axes[i]=axes[i];
        } else if(abs((int)axes[i])<=8192)last->axes[i]=0;
    }
    return active;
}
#endif
