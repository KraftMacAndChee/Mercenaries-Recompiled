/* Replay retail-execution regression expectations. See README.md and manifest.json.
 * This reader implements the fixture protocol, not the enemy-memory algorithm. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* input;
static unsigned record_index;
static void fail(const char* reason) {
    fprintf(stderr, "Enemy-memory fixture record %u: %s\n", record_index, reason);
    exit(1);
}
static uint32_t read_word(void) {
    unsigned char bytes[4];
    if (fread(bytes, 1, 4, input) != 4) fail("truncated record");
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static void expect(uint32_t actual) {
    if (read_word() != actual) fail("operation or input differs from recorded sequence");
}
static void operation(uint32_t opcode) {
    ++record_index;
    expect(opcode);
}
static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    return bits;
}
void oracle_open(const char* path) {
    char magic[8];
    if (sizeof(float) != 4 || sizeof(unsigned) != 4) fail("unsupported host ABI");
    input = fopen(path, "rb");
    if (!input) fail("cannot open expected results");
    if (fread(magic, 1, 8, input) != 8 || memcmp(magic, "ENMEM001", 8))
        fail("unsupported fixture format");
    record_index = 0;
}
void oracle_close(void) {
    if (fgetc(input) != EOF || ferror(input)) fail("unconsumed data or read error");
    if (fclose(input)) fail("close error");
    input = NULL;
}
void oracle_init(void) { operation(1); }
void oracle_update(float dt) { operation(2); expect(float_bits(dt)); }
float oracle_find(unsigned id) {
    uint32_t bits; float result;
    operation(3); expect(id); bits = read_word();
    memcpy(&result, &bits, 4);
    return result;
}
unsigned oracle_add(unsigned id, float timeout) {
    operation(4); expect(id); expect(float_bits(timeout));
    return read_word();
}
unsigned oracle_remove(unsigned id) {
    operation(5); expect(id);
    return read_word();
}
void* oracle_state(void) {
    static uint32_t state[9];
    unsigned i;
    operation(6);
    for (i = 0; i < 9; ++i) state[i] = read_word();
    return state;
}
