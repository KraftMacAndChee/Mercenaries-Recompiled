/* The retail 500 KiB limit is a scheduling budget, not an allocation size.
 * A completed buffer can remain pinned by an asset waiting on another read.
 * Admit one read beyond the budget only when disk work is idle; once bytes in
 * use reach the budget, the exception closes until consumers release them. */
#ifndef MERC_STREAM_ADMISSION_H
#define MERC_STREAM_ADMISSION_H
#include <stdint.h>
static inline int recomp_stream_idle_admission(uint32_t used, uint32_t request,
                                               uint32_t active, uint32_t queued)
{
    const uint32_t budget=500u*1024u;
    return !active && !queued && used<budget && request>0 &&
           request<=64u*1024u*1024u-used;
}
#endif
