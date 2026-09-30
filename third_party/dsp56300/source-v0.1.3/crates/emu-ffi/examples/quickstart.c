/*
 * Quickstart example for the dsp56300 C FFI.
 *
 * Compiled as part of the test suite to verify the C API works as documented.
 * See include/dsp56300.h for the full API reference.
 */

#include "dsp56300.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int quickstart_test(void)
{
    static uint32_t pram[4096];
    static uint32_t xram[4096];
    memset(pram, 0, sizeof(pram));
    memset(xram, 0, sizeof(xram));

    Dsp56300MemoryRegion p_regions[] = {{
        .start = 0, .end = 4096,
        .kind  = DSP56300_REGION_BUFFER,
        .data = { .buffer = { .base = pram, .offset = 0 } },
    }};
    Dsp56300MemoryRegion x_regions[] = {{
        .start = 0, .end = 4096,
        .kind  = DSP56300_REGION_BUFFER,
        .data = { .buffer = { .base = xram, .offset = 0 } },
    }};
    Dsp56300CreateInfo info = {
        .memory_map = {
            .x_regions = x_regions, .x_count = 1,
            .p_regions = p_regions, .p_count = 1,
        },
    };

    Dsp56300Jit *dsp = dsp56300_create(&info);
    assert(dsp != NULL);

    /* Load a JMP $100 and step one instruction */
    dsp56300_write_memory(dsp, DSP56300_MEM_SPACE_P, 0, 0x0C0100);
    dsp56300_step(dsp);

    Dsp56300State state;
    dsp56300_get_state(dsp, &state);
    assert(state.pc == 0x100);

    dsp56300_destroy(dsp);
    return 0;
}
