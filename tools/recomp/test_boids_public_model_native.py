"""Acceptance checks for the project-owned public-model flock replacement."""
from pathlib import Path
import os, shutil, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]

def test_public_flock():
    compiler=os.environ.get('CC') or shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
    source=r'''#include "boids.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static float n2(boid_vec v){return v.x*v.x+v.y*v.y+v.z*v.z;}
int main(void){
 const unsigned rates[]={30,60,90,120,240};
 boid_vec center={-840,50,510};boid_flock reference;
 boids_init(&reference,center,19);
 for(unsigned tick=0;tick<120;tick++)boids_advance(&reference,1.f/30);
 for(unsigned r=0;r<5;r++){
  boid_flock flock;boids_init(&flock,center,19);
  for(unsigned frame=0;frame<rates[r]*4;frame++)boids_advance(&flock,1.f/rates[r]);
  assert(!memcmp(reference.members,flock.members,sizeof(flock.members)));
 }
 float max_radius=0,min_pair=1e9f,max_speed=0,min_y=1e9f,max_y=-1e9f;
 double late_spread=0,heading_alignment=0;unsigned late_samples=0,heading_samples=0;
 for(unsigned seed=0;seed<16;seed++){
  boid_flock flock;boids_init(&flock,center,seed);
  unsigned far[15]={0},returned[15]={0};
  for(unsigned tick=0;tick<18000;tick++){
   boids_advance(&flock,1.f/30);
   if(tick>=1800 && tick%30==0){
    boid_vec c={0};for(unsigned i=0;i<RECOMP_BOID_COUNT;i++){
     c.x+=flock.members[i].position.x/RECOMP_BOID_COUNT;
     c.y+=flock.members[i].position.y/RECOMP_BOID_COUNT;
     c.z+=flock.members[i].position.z/RECOMP_BOID_COUNT;
    }
    boid_vec heading={0};
    for(unsigned i=0;i<RECOMP_BOID_COUNT;i++){
     boid_vec v=flock.members[i].velocity;float speed=sqrtf(n2(v));
     heading.x+=v.x/speed;heading.y+=v.y/speed;heading.z+=v.z/speed;
     boid_vec p=flock.members[i].position,d={p.x-c.x,p.y-c.y,p.z-c.z};
     late_spread+=sqrtf(n2(d));late_samples++;
    }
    heading_alignment+=sqrtf(n2(heading))/RECOMP_BOID_COUNT;heading_samples++;
   }
   for(unsigned i=0;i<RECOMP_BOID_COUNT;i++){
    boid_member b=flock.members[i];
    float dx=b.position.x-center.x,dy=b.position.y-center.y,dz=b.position.z-center.z;
    float radius=sqrtf(dx*dx+dz*dz),speed=sqrtf(n2(b.velocity));
    assert(isfinite(radius)&&isfinite(dy)&&isfinite(speed));
    if(radius>55)far[i]=1;
    if(far[i] && radius<25)returned[i]=1;
    assert(radius<125 && dy>0 && dy<24 && speed<6.501f);
    assert(b.velocity.x*b.velocity.x+b.velocity.z*b.velocity.z>6.249f);
    if(radius>max_radius)max_radius=radius;if(speed>max_speed)max_speed=speed;
    if(dy<min_y)min_y=dy;if(dy>max_y)max_y=dy;
    if(tick%30==0)for(unsigned j=i+1;j<RECOMP_BOID_COUNT;j++){
     boid_vec d={b.position.x-flock.members[j].position.x,b.position.y-flock.members[j].position.y,b.position.z-flock.members[j].position.z};
     float distance=sqrtf(n2(d));if(distance<min_pair)min_pair=distance;
    }
   }
  }
  for(unsigned i=0;i<15;i++)assert(far[i] && returned[i]);
 }
 // Check both spread and independent headings: spacing alone allows every
 // bird to follow the same path. Bounds allow loose subgroups to separate.
 assert(late_samples && late_spread/late_samples>6 && late_spread/late_samples<90);
 assert(heading_samples && heading_alignment/heading_samples<0.75);
 printf("PASS: mean normalized heading alignment %.3f (1 = all parallel)\n",heading_alignment/heading_samples);
 printf("PASS: sustained flock spread %.3f metres\n",late_spread/late_samples);
 boid_flock overlap;boids_init(&overlap,center,0);
 for(unsigned i=1;i<RECOMP_BOID_COUNT;i++)overlap.members[i]=overlap.members[0];
 for(unsigned tick=0;tick<300;tick++)boids_advance(&overlap,1.f/30);
 for(unsigned i=0;i<RECOMP_BOID_COUNT;i++)assert(isfinite(n2(overlap.members[i].position)));
 printf("PASS: 30/60/90/120/240 Hz identical; 16 seeds x 600 seconds; radius %.3f, speed %.3f, height %.3f..%.3f, sampled closest pair %.3f; coincident starts finite\n",max_radius,max_speed,min_y,max_y,min_pair);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='public-boids-') as directory:
        folder=Path(directory);(folder/'test.c').write_text(source)
        subprocess.run([compiler,'-std=c11','-O2','-I'+str(ROOT/'ports/mercenaries/src'),str(folder/'test.c'),str(ROOT/'ports/mercenaries/src/boids.c'),'-lm','-o',str(folder/'test.exe')],check=True)
        subprocess.run([str(folder/'test.exe')],check=True)
if __name__=='__main__':test_public_flock()
