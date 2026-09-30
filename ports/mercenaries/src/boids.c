/* Project-owned flock steering using the public separation/alignment/cohesion
 * model described at https://www.red3d.com/cwr/boids/ . No original steering
 * implementation is used here. See docs/runtime/independent-replacements.md
 * for the replacement history, chosen tuning and validation limits.
 * The guest actor bridge and 15-bird F9 interface remain separate. */
#include "boids.h"
#include <math.h>
#include <string.h>

static boid_vec vector(float x, float y, float z)
{
    boid_vec result = {x, y, z};
    return result;
}
static boid_vec plus(boid_vec a, boid_vec b)
{
    return vector(a.x+b.x, a.y+b.y, a.z+b.z);
}
static boid_vec difference(boid_vec a, boid_vec b)
{
    return vector(a.x-b.x, a.y-b.y, a.z-b.z);
}
static boid_vec scale(boid_vec a, float factor)
{
    return vector(a.x*factor, a.y*factor, a.z*factor);
}
static float norm2(boid_vec a)
{
    return a.x*a.x+a.y*a.y+a.z*a.z;
}
static boid_vec limit(boid_vec value, float maximum)
{
    float n=norm2(value);
    return n>maximum*maximum ? scale(value, maximum/sqrtf(n)) : value;
}
static float sample(boid_flock* flock)
{
    /* Local deterministic generator: never consume the game's random stream. */
    flock->random=1664525u*flock->random+1013904223u;
    return (float)(flock->random>>8)*(1.0f/16777216.0f);
}
static void choose_destination(boid_flock* flock, unsigned bird)
{
    float angle, radius;
    if (flock->outbound[bird]) {
        /* Continue into the distance before turning back, instead of endlessly
         * circling the spawn area. Each bird retains its own departure heading. */
        boid_vec velocity=flock->members[bird].velocity;
        angle=atan2f(velocity.z,velocity.x)+(sample(flock)-0.5f);
        radius=65.0f+30.0f*sample(flock);
        flock->goal_ticks[bird]=900u+(unsigned)(300.0f*sample(flock));
    } else {
        angle=sample(flock)*6.28318530718f;
        radius=10.0f+15.0f*sample(flock);
        flock->goal_ticks[bird]=750u+(unsigned)(300.0f*sample(flock));
    }
    flock->goals[bird]=plus(flock->center,
        vector(radius*cosf(angle),6.0f+10.0f*sample(flock),radius*sinf(angle)));
}

void boids_init(boid_flock* flock, boid_vec center, uint32_t seed)
{
    unsigned i;
    memset(flock,0,sizeof(*flock));
    flock->center=center;
    flock->random=seed;
    for(i=0;i<RECOMP_BOID_COUNT;i++) {
        float angle=6.28318530718f*((float)i+0.15f*sample(flock))/RECOMP_BOID_COUNT;
        float radius=10.0f+2.0f*sample(flock);
        flock->members[i].position=plus(center,
            vector(radius*cosf(angle),8.0f+4.0f*sample(flock),radius*sinf(angle)));
        flock->members[i].velocity=vector(-4.5f*sinf(angle),0,4.5f*cosf(angle));
    }
    for(i=0;i<RECOMP_BOID_COUNT;i++) {
        choose_destination(flock,i);
        flock->goal_ticks[i]=90u+(unsigned)(180.0f*sample(flock));
    }
}

static void advance_tick(boid_flock* flock)
{
    boid_member output[RECOMP_BOID_COUNT];
    unsigned i,j;
    for(i=0;i<RECOMP_BOID_COUNT;i++) {
        boid_member bird=flock->members[i];
        boid_vec separation={0}, heading={0}, gathering={0};
        boid_vec desired, acceleration, local;
        unsigned neighbors=0;
        /* Independent goals and staggered turns let the flock split and rejoin.
         * Neighbor steering stays local instead of locking all birds to a leader. */
        if (!flock->goal_ticks[i] || norm2(difference(bird.position,flock->goals[i]))<36.0f) {
            flock->outbound[i]=!flock->outbound[i];
            choose_destination(flock,i);
        }
        --flock->goal_ticks[i];
        for(j=0;j<RECOMP_BOID_COUNT;j++) {
            boid_vec away;
            float distance2;
            if(i==j) continue;
            away=difference(bird.position,flock->members[j].position);
            distance2=norm2(away);
            if(distance2<12.0f*12.0f) {
                heading=plus(heading,flock->members[j].velocity);
                gathering=plus(gathering,flock->members[j].position);
                neighbors++;
            }
            /* Keep a visible flock spread instead of collapsing into one knot. */
            if(distance2<6.0f*6.0f) {
                if(distance2<0.0001f) {
                    /* Deterministic antisymmetric escape for coincident birds. */
                    away=vector(i<j?-1.0f:1.0f,0,0);
                    distance2=1.0f;
                }
                separation=plus(separation,scale(away,1.0f/(0.25f+distance2)));
            }
        }
        desired=scale(difference(flock->goals[i],bird.position),0.30f);
        if(neighbors) {
            desired=plus(desired,scale(heading,0.25f/neighbors));
            desired=plus(desired,scale(difference(scale(gathering,1.0f/neighbors),bird.position),0.06f));
        }
        desired=plus(desired,scale(separation,18.0f));
        local=difference(bird.position,flock->center);
        /* Soft containment starts well before the visible flight-area edge. */
        if(local.x>108) desired.x-=1.2f*(local.x-108);
        if(local.x< -108) desired.x-=1.2f*(local.x+108);
        if(local.z>108) desired.z-=1.2f*(local.z-108);
        if(local.z< -108) desired.z-=1.2f*(local.z+108);
        desired.y+=0.8f*(10.0f-local.y);
        desired.y=fmaxf(-1.5f,fminf(1.5f,desired.y));
        desired=limit(desired,6.5f);
        acceleration=limit(scale(difference(desired,bird.velocity),1.8f),4.0f);
        bird.velocity=limit(plus(bird.velocity,scale(acceleration,1.0f/30.0f)),6.5f);
        {
            float horizontal=bird.velocity.x*bird.velocity.x+bird.velocity.z*bird.velocity.z;
            if(horizontal<2.5f*2.5f) {
                float x=bird.velocity.x,z=bird.velocity.z;
                if(horizontal<0.0001f) { x=1;z=0;horizontal=1; }
                bird.velocity.x=x*2.5f/sqrtf(horizontal);
                bird.velocity.z=z*2.5f/sqrtf(horizontal);
            }
        }
        bird.position=plus(bird.position,scale(bird.velocity,1.0f/30.0f));
        output[i]=bird;
    }
    memcpy(flock->members,output,sizeof(output));
}

unsigned boids_advance(boid_flock* flock, float seconds)
{
    unsigned ticks=0;
    if(!isfinite(seconds) || seconds<=0) return 0;
    flock->remainder+=fmin((double)seconds,0.25);
    while(flock->remainder+1.e-8>=1.0/30.0) {
        advance_tick(flock);
        flock->remainder-=1.0/30.0;
        ticks++;
    }
    return ticks;
}
