/* Failed voice requests must finish their caller's Lua script before invoking
 * its continuation. The retail bouncer defines PreBriefing after requesting VO.
 * This queue lives only through the current protected script call, not a timer. */
/* Single-translation-unit implementation owned by recomp_manual.c. Static
 * queue state is shared by nested guest script calls, not synchronized for
 * concurrent host callers. Tests include it in an isolated fixture. */
#ifndef MERC_VOICE_CALLBACK_QUEUE_H
#define MERC_VOICE_CALLBACK_QUEUE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct VoiceContinuation {
    struct VoiceContinuation *next;
    uint64_t serial;
    uint32_t owner, vm;
    char name[1];
} VoiceContinuation;
static VoiceContinuation *voice_head, *voice_tail;
static uint64_t voice_serial;
static unsigned voice_depth;
static int voice_draining;
static uint64_t voice_call_begin(void) { ++voice_depth; return voice_serial; }
static int voice_defer(uint32_t owner, uint32_t vm, const char *name) {
    if (!voice_depth || !owner || !vm || !name || !*name) return 0;
    size_t length = strlen(name);
    VoiceContinuation *item = (VoiceContinuation *)malloc(sizeof(*item) + length);
    if (!item) return 0;
    item->next = NULL; item->serial = ++voice_serial;
    item->owner = owner; item->vm = vm;
    memcpy(item->name, name, length + 1);
    if (voice_tail) voice_tail->next = item; else voice_head = item;
    voice_tail = item;
    return 1;
}
static void voice_discard(uint32_t owner, uint64_t after, int failed_call) {
    VoiceContinuation **link = &voice_head;
    voice_tail = NULL;
    while (*link) {
        VoiceContinuation *item = *link;
        if ((failed_call && item->serial > after) || (!failed_call && item->owner == owner)) {
            *link = item->next; free(item);
        } else { voice_tail = item; link = &item->next; }
    }
}
static void voice_call_end(uint64_t mark, int success,
                           void (*dispatch)(uint32_t, uint32_t, const char *)) {
    if (!success) voice_discard(0, mark, 1);
    if (!voice_depth || --voice_depth || voice_draining) return;
    voice_draining = 1;
    while (voice_head) {
        VoiceContinuation *item = voice_head;
        voice_head = item->next;
        if (!voice_head) voice_tail = NULL;
        /* Unlink first: a callback may close a script or queue another VO. */
        dispatch(item->owner, item->vm, item->name);
        free(item);
    }
    voice_draining = 0;
}
#endif
