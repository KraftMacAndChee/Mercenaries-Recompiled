"""Exercise decoupled turrets' host-side free-camera state.
Guest integration (camera matrix, turret drive, reticle) lives in
decoupled_turret_guest.h and needs a live retail session to observe.
"""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=r"""
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "decoupled_turret.c"
int main(void){
 /* Decoupled turrets: seeded from the retail view, mouse yaw turns toward the
    camera's screen-right axis whatever the world handedness, pitch clamps,
    and a controller stick hands control back to retail. */
 g_turret_enabled=1;float f[3],barrel[3]={0,0,1},right[3]={1,0,0},left[3]={-1,0,0};
 assert(!decoupled_turret_direction(f));decoupled_turret_seed(barrel);
 decoupled_turret_mouse(.1f,0);decoupled_turret_apply_input(right);decoupled_turret_direction(f);assert(f[0]>0);
 decoupled_turret_seed(barrel);decoupled_turret_mouse(.1f,0);decoupled_turret_apply_input(left);decoupled_turret_direction(f);assert(f[0]<0);
 decoupled_turret_seed(barrel);decoupled_turret_mouse(0,5.f);decoupled_turret_apply_input(right);decoupled_turret_direction(f);
 assert(fabsf(asinf(f[1])-1.40f)<1e-3f);
 assert(!recomp_decoupled_turret_live());decoupled_turret_note_camera(0x1234);decoupled_turret_mouse(0,0);
 assert(recomp_decoupled_turret_live());decoupled_turret_controller_input();
 assert(!recomp_decoupled_turret_live() && !decoupled_turret_direction(f));
 g_turret_enabled=0;decoupled_turret_mouse(0,0);assert(!recomp_decoupled_turret_live());
 /* Helicopter pilot: seeded behind the helicopter, mouse turns the camera
    yaw in the camera's right-turn sense, the helicopter is steered with the
    retail TurnTo rule (error * pi, clamped), and only turrets RsAiPlayer is
    aiming for the player are driven. */
 g_heli_enabled=1;float yaw=0;
 assert(!decoupled_heli_camera(0x5000,.2f,1.f,&yaw));          /* seed frame stays retail */
 decoupled_heli_mouse(.1f);assert(decoupled_heli_camera(0x5000,.2f,1.f,&yaw));
 assert(fabsf(yaw-.3f)<1e-5f);
 assert(fabsf(decoupled_heli_steer()-.1f*3.14159265f)<1e-4f);   /* camera .1 rad right of heading */
 decoupled_heli_mouse(1.f);decoupled_heli_camera(0x5000,.2f,1.f,&yaw);assert(decoupled_heli_steer()==1.f);
 decoupled_heli_mouse(0);decoupled_heli_camera(0x5000,yaw,-1.f,&yaw);assert(decoupled_heli_steer()==0.f);
 decoupled_heli_mouse(.05f);decoupled_heli_camera(0x5000,yaw,-1.f,&yaw);   /* mirrored sense */
 assert(decoupled_heli_steer()>0.f);
 assert(!decoupled_heli_turret(0x7000));decoupled_heli_note_turret(0x7000);assert(decoupled_heli_turret(0x7000));
 assert(recomp_decoupled_vehicle_live());
 decoupled_heli_controller_input();assert(!recomp_decoupled_heli_live() && decoupled_heli_steer()==0.f);
 puts("decoupled turret ok");return 0;}
"""
with tempfile.TemporaryDirectory(prefix='merc-decoupled-turret-') as folder:
 d=Path(folder);(d/'test.c').write_text(source,encoding='utf-8');exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I'+str(ROOT/'ports/mercenaries/src'),str(d/'test.c'),'-o',str(exe),'-lm'],check=True)
 subprocess.run([str(exe)],check=True)
