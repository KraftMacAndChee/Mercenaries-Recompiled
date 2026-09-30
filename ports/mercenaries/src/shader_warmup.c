#include "shader_warmup.h"
#include "d3d8_combiners.h"
#include "d3d8_vsh.h"
#include "preview_log.h"
#include <stdlib.h>
#include "shader_warmup_catalog.inc"

static void warmup(void)
{
    const ULONGLONG begin = GetTickCount64();
    unsigned ready = 0;
    if (getenv("MERCENARIES_DISABLE_SHADER_WARMUP")) return;
    for (unsigned i = 0; i < sizeof(warmup_states)/sizeof(warmup_states[0]); ++i)
        ready += d3d8_combiners_get_shader(&warmup_states[i]) != NULL;
    /* A failed warm-up leaves the ordinary first-use path available. Never
     * bind a shader or change draw constants here. Disk cache identity remains
     * the complete generated source, so renderer changes invalidate it. */
    xbox_preview_log_event("shader-warmup", "ready=%u total=%u elapsed_ms=%llu",
        ready, (unsigned)(sizeof(warmup_states)/sizeof(warmup_states[0])),
        (unsigned long long)(GetTickCount64()-begin));
}
static void warmup_vertex(void)
{
    const ULONGLONG begin = GetTickCount64();
    unsigned ready = 0;
    if (getenv("MERCENARIES_DISABLE_SHADER_WARMUP")) return;
    for (unsigned i = 0; i < sizeof(warmup_vertex_states)/sizeof(warmup_vertex_states[0]); ++i)
        ready += d3d8_vsh_prewarm(&warmup_vertex_states[i]) != FALSE;
    xbox_preview_log_event("shader-warmup-vertex", "ready=%u vertex_total=%u elapsed_ms=%llu",
        ready, (unsigned)(sizeof(warmup_vertex_states)/sizeof(warmup_vertex_states[0])),
        (unsigned long long)(GetTickCount64()-begin));
}
void recomp_register_shader_warmup(void)
{
    d3d8_combiners_set_warmup(warmup);
    d3d8_vsh_set_warmup(warmup_vertex);
}
