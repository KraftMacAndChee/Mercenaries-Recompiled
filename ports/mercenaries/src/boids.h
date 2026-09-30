#ifndef MERCENARIES_BOIDS_H
#define MERCENARIES_BOIDS_H
#include <stdint.h>
#define RECOMP_BOID_COUNT 15
typedef struct { float x,y,z; } boid_vec;
typedef struct { boid_vec position, velocity; } boid_member;
typedef struct {
    boid_member members[RECOMP_BOID_COUNT];
    boid_vec center, goals[RECOMP_BOID_COUNT];
    unsigned goal_ticks[RECOMP_BOID_COUNT];
    unsigned outbound[RECOMP_BOID_COUNT];
    uint32_t random;
    double remainder;
} boid_flock;
void boids_init(boid_flock *flock, boid_vec center, uint32_t seed);
unsigned boids_advance(boid_flock *flock, float seconds);
#endif
