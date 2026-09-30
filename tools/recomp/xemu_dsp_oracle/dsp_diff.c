/* Development-only differential oracle.  Xemu's GPL interpreter is compiled
 * into this standalone diagnostic, never into the MIT port executable. */
#include <dsp56300.h>
#include <stdlib.h>
#include <string.h>
#include "dsp_cpu.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef struct PeripheralState {
    Dsp56300Jit *jit;
    dsp_core_t *interp;
    uint32_t regs[4];
    uint32_t interrupts;
    unsigned reads;
    unsigned writes;
    uint32_t last_address;
    uint32_t last_value;
    unsigned dma_read_count;
    bool dma_eol;
} PeripheralState;

typedef struct DSPDiagnosticSnapshot {
    char magic[8];
    Dsp56300State state;
    uint32_t xram[DSP_XRAM_SIZE];
    uint32_t yram[DSP_YRAM_SIZE];
    uint32_t pram[DSP_PRAM_SIZE];
    uint32_t interrupts;
    uint32_t dma_configuration;
    uint32_t dma_control;
    uint32_t dma_start_block;
    uint32_t dma_next_block;
    uint32_t dma_eol;
    uint32_t dma_read_count;
} DSPDiagnosticSnapshot;

static uint32_t peripheral_read_common(PeripheralState *p, uint32_t address)
{
    p->reads++;
    p->last_address = address;
    switch (address) {
    case 0xffffb3: return 0;
    case 0xffffc5: return p->interrupts | (p->dma_eol ? 0x80u : 0u);
    case 0xffffd4: return p->regs[0];
    case 0xffffd5: return p->regs[1];
    case 0xffffd6:
        if (p->regs[2] & 0x10u) {
            if (++p->dma_read_count > 2u) {
                p->regs[2] &= ~0x10u;
                p->regs[2] |= 0x20u;
                p->dma_read_count = 0;
            }
        }
        return p->regs[2];
    case 0xffffd7: return p->regs[3];
    default: return 0x0ababa;
    }
}

static void peripheral_write_common(PeripheralState *p, uint32_t address,
                                    uint32_t value)
{
    p->writes++;
    p->last_address = address;
    p->last_value = value & 0x00ffffff;
    if (address == 0xffffc5) {
        p->interrupts &= ~value;
        if (value & 0x80u) p->dma_eol = false;
    }
    else if (address >= 0xffffd4 && address <= 0xffffd7)
        p->regs[address - 0xffffd4] = value & 0x00ffffff;
}

static uint32_t jit_read(void *opaque, uint32_t address)
{ return peripheral_read_common((PeripheralState *)opaque, address); }
static void jit_write(void *opaque, uint32_t address, uint32_t value)
{
    PeripheralState *p = (PeripheralState *)opaque;
    peripheral_write_common(p, address, value);
    if (address == 0xffffc4 && (value & 1))
        dsp56300_set_halt_requested(p->jit, true);
}
static uint32_t interp_read(dsp_core_t *core, uint32_t address)
{ return peripheral_read_common((PeripheralState *)core->opaque, address); }
static void interp_write(dsp_core_t *core, uint32_t address, uint32_t value)
{
    PeripheralState *p = (PeripheralState *)core->opaque;
    peripheral_write_common(p, address, value);
    if (address == 0xffffc4 && (value & 1)) core->is_idle = true;
}

static int load_u24be(const char *path, uint32_t *dst, size_t count)
{
    FILE *f = fopen(path, "rb");
    size_t i;
    if (!f) return 0;
    for (i = 0; i < count; i++) {
        unsigned char b[3];
        if (fread(b, 1, 3, f) != 3) break;
        dst[i] = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    }
    fclose(f);
    return i == count;
}

static int load_snapshot(const char *path, DSPDiagnosticSnapshot *snapshot)
{
    FILE *f = fopen(path, "rb");
    int ok;
    if (!f) return 0;
    ok = fread(snapshot, sizeof(*snapshot), 1, f) == 1 &&
         memcmp(snapshot->magic, "DSPSS001", 8) == 0;
    fclose(f);
    return ok;
}

static void load_interp_from_jit_state(dsp_core_t *c,
                                       const Dsp56300State *s)
{
    unsigned i;
    c->pc = s->pc & 0xffffu;
    c->cycle_count = s->cycle_count;
    c->loop_rep = s->loop_rep;
    c->pc_on_rep = s->pc_on_rep;
    c->cur_inst_len = (uint16_t)s->pc_advance;
    memcpy(c->registers, s->registers, sizeof(c->registers));
    memcpy(c->stack, s->stack, sizeof(c->stack));
    for (i = DSP_REG_R0; i <= DSP_REG_R7; i++) c->registers[i] &= 0xffffu;
    for (i = DSP_REG_N0; i <= DSP_REG_N7; i++) c->registers[i] &= 0xffffu;
    for (i = DSP_REG_M0; i <= DSP_REG_M7; i++) {
        if ((c->registers[i] & 0xffffffu) == 0xffffffu)
            c->registers[i] = 0xffffu;
        else
            c->registers[i] &= 0xffffu;
    }
    c->registers[DSP_REG_SR] &= 0xffffu;
    c->registers[DSP_REG_OMR] &= 0xffu;
    c->registers[DSP_REG_SP] &= 0x3fu;
    c->registers[DSP_REG_SSH] &= 0xffffu;
    c->registers[DSP_REG_SSL] &= 0xffffu;
    c->registers[DSP_REG_LA] &= 0xffffu;
    c->registers[DSP_REG_LC] &= 0xffffu;
    for (i = 0; i < 16; i++) {
        c->stack[0][i] &= 0xffffu;
        c->stack[1][i] &= 0xffffu;
    }
}

static uint32_t normalized_interp_reg(const dsp_core_t *c, unsigned r)
{
    uint32_t v = c->registers[r];
    if (r == DSP_REG_SR || r == DSP_REG_SSL)
        v &= 0x0000ff7f;
    return v & 0x00ffffff;
}

static uint32_t normalized_jit_reg(const Dsp56300State *j, unsigned r)
{
    uint32_t v = j->registers[r] & 0x00ffffff;
    if ((r >= DSP_REG_R0 && r <= DSP_REG_M7) ||
        r == DSP_REG_SSH ||
        r == DSP_REG_LA || r == DSP_REG_LC)
        v &= 0x0000ffff;
    else if (r == DSP_REG_SSL)
        v &= 0x0000ff7f;
    else if (r == DSP_REG_OMR)
        v &= 0x000000ff;
    else if (r == DSP_REG_SP)
        v &= 0x0000003f;
    else if (r == DSP_REG_SR)
        v &= 0x0000ff7f;
    return v;
}

/* Match the state produced by Xemu's long-standing MCPX C interpreter reset,
 * then converted by dsp_jit_sync_from_vm().  The generic 56300 library reset
 * intentionally models the processor's architectural reset defaults instead;
 * comparing those two unrelated starting states only obscures real execution
 * differences in the Xbox firmware. */
static void normalize_jit_to_xemu_reset(Dsp56300Jit *jit)
{
    static const unsigned interrupt_slots[4] = { 0, 2, 1, 4 };
    Dsp56300State s;
    unsigned i;

    dsp56300_get_state(jit, &s);
    memset(&s, 0, sizeof(s));
    s.registers[DSP_REG_OMR] = 0x000002;
    for (i = 0; i < 8; i++)
        s.registers[DSP_REG_M0 + i] = 0x00ffffff;
    s.interrupts.vector_addr = 0x0000ffff;
    s.interrupts.saved_pc = 0x0000ffff;
    for (i = 0; i < ARRAY_SIZE(interrupt_slots); i++)
        s.interrupts.ipl[interrupt_slots[i]] = 3;
    dsp56300_set_state(jit, &s);
    dsp56300_invalidate_cache(jit);
}

static int compare_state(unsigned step, uint32_t source_pc, uint32_t opcode,
                         const Dsp56300State *j, const dsp_core_t *c,
                         const uint32_t *jx, const uint32_t *jy,
                         const uint32_t *jp_mem,
                         const PeripheralState *jp, const PeripheralState *cp)
{
    static const unsigned regs[] = {
        4,5,6,7,8,9,10,11,12,13,
        16,17,18,19,20,21,22,23,
        24,25,26,27,28,29,30,31,
        32,33,34,35,36,37,38,39,
        57,58,59,60,61,62,63
    };
    size_t i;
    if ((j->pc & 0xffff) != (c->pc & 0xffff)) {
        printf("DIVERGE step=%u source_pc=%06X opcode=%06X pc jit=%06X xemu=%06X\n",
               step, source_pc, opcode, j->pc, c->pc); return 0;
    }
    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        unsigned r = regs[i];
        uint32_t a = normalized_jit_reg(j, r);
        uint32_t b = normalized_interp_reg(c, r);
        if (a != b) {
            printf("DIVERGE step=%u source_pc=%06X opcode=%06X reg=%u jit=%06X xemu=%06X pc=%06X\n",
                   step, source_pc, opcode, r, a, b, j->pc); return 0;
        }
    }
    if (jp->reads != cp->reads || jp->writes != cp->writes ||
        jp->last_address != cp->last_address || jp->last_value != cp->last_value) {
        printf("DIVERGE step=%u opcode=%06X peripheral jit=%u/%u/%06X/%06X xemu=%u/%u/%06X/%06X pc=%06X\n",
               step, opcode, jp->reads, jp->writes, jp->last_address, jp->last_value,
               cp->reads, cp->writes, cp->last_address, cp->last_value, j->pc);
        return 0;
    }
    for (i = 0; i < DSP_XRAM_SIZE; i++) {
        if ((jx[i] & 0xffffff) != (c->xram[i] & 0xffffff)) {
            printf("DIVERGE step=%u opcode=%06X X:%04X jit=%06X xemu=%06X pc=%06X\n",
                   step, opcode, (unsigned)i, jx[i] & 0xffffff,
                   c->xram[i] & 0xffffff, j->pc); return 0;
        }
    }
    for (i = 0; i < DSP_YRAM_SIZE; i++) {
        if ((jy[i] & 0xffffff) != (c->yram[i] & 0xffffff)) {
            printf("DIVERGE step=%u opcode=%06X Y:%04X jit=%06X xemu=%06X pc=%06X\n",
                   step, opcode, (unsigned)i, jy[i] & 0xffffff,
                   c->yram[i] & 0xffffff, j->pc); return 0;
        }
    }
    for (i = 0; i < DSP_PRAM_SIZE; i++) {
        if ((jp_mem[i] & 0xffffff) != (c->pram[i] & 0xffffff)) {
            printf("DIVERGE step=%u source_pc=%06X opcode=%06X P:%04X jit=%06X xemu=%06X pc=%06X\n",
                   step, source_pc, opcode, (unsigned)i,
                   jp_mem[i] & 0xffffff, c->pram[i] & 0xffffff, j->pc);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    uint32_t jx[DSP_XRAM_SIZE], jy[DSP_YRAM_SIZE], jp_mem[DSP_PRAM_SIZE];
    dsp_core_t c;
    PeripheralState jp = {0}, cp = {0};
    Dsp56300MemoryRegion xr[3] = {0}, yr[2] = {0}, pr[1] = {0};
    Dsp56300CreateInfo info = {0};
    Dsp56300State js = {0};
    DSPDiagnosticSnapshot snapshot;
    bool snapshot_mode;
    unsigned step, phase = 0;
    unsigned limit = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 200000;
    if (argc < 2) { fprintf(stderr, "usage: dsp_diff ep_pram.u24be|state.bin [steps]\n"); return 2; }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot_mode = load_snapshot(argv[1], &snapshot) != 0;
    memset(jx, 0xca, sizeof(jx)); memset(jy, 0xca, sizeof(jy));
    memset(jp_mem, 0xca, sizeof(jp_mem)); memset(&c, 0, sizeof(c));
    memset(c.xram, 0xca, sizeof(c.xram)); memset(c.yram, 0xca, sizeof(c.yram));
    memset(c.pram, 0xca, sizeof(c.pram));
    if (snapshot_mode) {
        FILE *dump;
        printf("SNAPSHOT pc=%06X state_bytes=%u\n", snapshot.state.pc,
               (unsigned)sizeof(snapshot.state));
        if (argc > 3 && (dump = fopen(argv[3], "wb")) != NULL) {
            fwrite(snapshot.pram, sizeof(snapshot.pram), 1, dump);
            fclose(dump);
        }
        memcpy(jx, snapshot.xram, sizeof(jx));
        memcpy(jy, snapshot.yram, sizeof(jy));
        memcpy(jp_mem, snapshot.pram, sizeof(jp_mem));
        memcpy(c.xram, snapshot.xram, sizeof(c.xram));
        memcpy(c.yram, snapshot.yram, sizeof(c.yram));
        memcpy(c.pram, snapshot.pram, sizeof(c.pram));
        phase = 2;
    } else if (!load_u24be(argv[1], jp_mem, DSP_PRAM_SIZE) ||
               !load_u24be(argv[1], c.pram, DSP_PRAM_SIZE)) return 3;
    xr[0]=(Dsp56300MemoryRegion){0,0x1000,DSP56300_REGION_BUFFER,.data.buffer={jx,0}};
    xr[1]=(Dsp56300MemoryRegion){0x1400,0x1800,DSP56300_REGION_BUFFER,.data.buffer={jx,0x0c00}};
    xr[2]=(Dsp56300MemoryRegion){0xffff80,0x1000000,DSP56300_REGION_CALLBACK,.data.callback={&jp,jit_read,jit_write}};
    yr[0]=(Dsp56300MemoryRegion){0,0x0800,DSP56300_REGION_BUFFER,.data.buffer={jy,0}};
    /* Xemu's production C interpreter stores YRAM immediately before PRAM.
     * Its MCPX EP firmware intentionally reaches Y:$0800-$17ff, which therefore
     * resolves to the PRAM backing at offset zero.  Express that layout as an
     * explicit region in the JIT half of this development-only oracle so the
     * comparison remains bounds-safe and can expose the next semantic mismatch. */
    yr[1]=(Dsp56300MemoryRegion){0x0800,0x1800,DSP56300_REGION_BUFFER,.data.buffer={jp_mem,0}};
    pr[0]=(Dsp56300MemoryRegion){0,0x1000,DSP56300_REGION_BUFFER,.data.buffer={jp_mem,0}};
    info.memory_map=(Dsp56300MemoryMap){xr,3,yr,2,pr,1};
    info.address_register_mask = 0x0000ffffu;
    jp.jit=dsp56300_create(&info); if (!jp.jit) return 4;
    dsp56300_reset(jp.jit);
    if (snapshot_mode) {
        snapshot.state.halt_requested = false;
        dsp56300_set_state(jp.jit, &snapshot.state);
        dsp56300_invalidate_cache(jp.jit);
    } else {
        normalize_jit_to_xemu_reset(jp.jit);
    }
    c.opaque=&cp; c.read_peripheral=interp_read; c.write_peripheral=interp_write;
    cp.interp=&c; dsp56k_reset_cpu(&c); c.is_idle=false;
    if (snapshot_mode) {
        load_interp_from_jit_state(&c, &snapshot.state);
        jp.interrupts=cp.interrupts=snapshot.interrupts;
        jp.regs[0]=cp.regs[0]=snapshot.dma_next_block;
        jp.regs[1]=cp.regs[1]=snapshot.dma_start_block;
        jp.regs[2]=cp.regs[2]=snapshot.dma_control;
        jp.regs[3]=cp.regs[3]=snapshot.dma_configuration;
        jp.dma_eol=cp.dma_eol=snapshot.dma_eol != 0;
        jp.dma_read_count=cp.dma_read_count=snapshot.dma_read_count;
    } else {
        jp.interrupts=cp.interrupts=2;
    }
    for(step=1; step<=limit; step++) {
        uint32_t pc, opcode, pre_jit_sr, pre_xemu_sr;
        dsp56300_get_state(jp.jit,&js); pc=js.pc; opcode=jp_mem[pc & 0xfff] & 0xffffff;
        pre_jit_sr = normalized_jit_reg(&js, DSP_REG_SR);
        pre_xemu_sr = normalized_interp_reg(&c, DSP_REG_SR);
        dsp56300_step(jp.jit);
        dsp56k_execute_instruction(&c); c.cycle_count += c.instr_cycle;
        dsp56300_get_state(jp.jit,&js);
        if (snapshot_mode && step >= 190 && step <= 201) {
            printf("TRACE step=%u pc=%06X opcode=%06X sr jit=%06X->%06X xemu=%06X->%06X omr=%06X/%06X r0=%06X/%06X n0=%06X/%06X m0=%06X/%06X a1=%06X/%06X\n",
                   step, pc, opcode, pre_jit_sr,
                   normalized_jit_reg(&js, DSP_REG_SR), pre_xemu_sr,
                   normalized_interp_reg(&c, DSP_REG_SR),
                   normalized_jit_reg(&js, DSP_REG_OMR), normalized_interp_reg(&c, DSP_REG_OMR),
                   normalized_jit_reg(&js, DSP_REG_R0), normalized_interp_reg(&c, DSP_REG_R0),
                   normalized_jit_reg(&js, DSP_REG_N0), normalized_interp_reg(&c, DSP_REG_N0),
                   normalized_jit_reg(&js, DSP_REG_M0), normalized_interp_reg(&c, DSP_REG_M0),
                   normalized_jit_reg(&js, DSP_REG_A1), normalized_interp_reg(&c, DSP_REG_A1));
        }
        if (!compare_state(step,pc,opcode,&js,&c,jx,jy,jp_mem,&jp,&cp)) break;
        if (dsp56300_halt_requested(jp.jit) || c.is_idle) {
            bool jit_halt = dsp56300_halt_requested(jp.jit);
            if (jit_halt != c.is_idle) {
                printf("DIVERGE step=%u opcode=%06X halt jit=%u xemu=%u pc=%06X\n",
                       step, opcode, jit_halt, c.is_idle, js.pc);
                break;
            }
            if (phase++ < 2) {
                printf("STARTUP-HALT phase=%u step=%u pc=%06X; raising frame interrupt\n",
                       phase, step, js.pc);
                dsp56300_set_halt_requested(jp.jit, false);
                c.is_idle = false;
                jp.interrupts = cp.interrupts = 2;
                continue;
            }
            printf("FRAME-HALT step=%u jit=%u xemu=%u pc=%06X\n", step,
                   jit_halt, c.is_idle, js.pc); break;
        }
    }
    if (step>limit) printf("MATCH steps=%u pc=%06X\n",limit,js.pc);
    if (argc > 4) {
        FILE *dump = fopen(argv[4], "wb");
        if (dump != NULL) {
            fwrite(jp_mem, sizeof(jp_mem), 1, dump);
            fclose(dump);
        }
    }
    dsp56300_destroy(jp.jit);
    return 0;
}
