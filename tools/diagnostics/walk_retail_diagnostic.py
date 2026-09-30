"""Bounded normal-stick walking in one explicitly selected hidden diagnostic.

Reads player/camera state, writes only the existing gamepad command protocol.
Never writes guest memory, uses global input, changes focus, or targets preview.
Stops at dialogs, invalid state, arrival, lack of movement, or timeout.
"""
import argparse
import ctypes
from ctypes import wintypes
import math
import json
import os
from pathlib import Path
import struct
import tempfile
import time
from inspect_live_named_actors import project_world_point

ROOT = Path(__file__).resolve().parents[2]


def capture_acknowledged(directory, sequence):
    """Accept the exact capture or an older build's numeric flip-series suffix.

    The exact sequence is mandatory: another command's capture cannot ack ours.
    Old builds appended the flip counter even to explicitly named captures.
    """
    basename = f'gamepad-{sequence:06d}.bmp'
    if (directory / basename).is_file():
        return True
    for path in directory.glob(basename + '-*.bmp'):
        suffix = path.name[len(basename) + 1:-4]
        if suffix.isascii() and suffix.isdecimal() and path.is_file():
            return True
    return False


def validate_waypoints(value):
    if not isinstance(value, list) or not 1 <= len(value) <= 64:
        raise ValueError('Route must contain 1..64 ordinary walking destinations')
    result = []
    for point in value:
        if not isinstance(point, list) or len(point) != 2:
            raise ValueError('Each waypoint must be [world_x, world_z]')
        if any(type(v) not in (int, float) or not math.isfinite(v) or abs(v) > 10000 for v in point):
            raise ValueError('Waypoint coordinates must be finite and within world bounds')
        result.append(tuple(point))
    return result


def trigger_renewal_ms(elapsed,duration):
    """Finite ordinary trigger hold; each local pulse expires if the helper dies."""
    if not math.isfinite(elapsed) or elapsed<0 or not math.isfinite(duration) or not .1<=duration<=30:
        raise ValueError('Invalid bounded trigger interval')
    return max(0,min(1500,math.ceil((duration-elapsed)*1000)))


def aim_errors(inverse, frustum, target, zoom=1.0):
    # RedCamera::GetFrustum returns stored width/height divided by zoom.
    # The live laser camera uses 1.5x; raw +90 fields alone mis-aim its ray.
    if len(frustum) != 4:
        raise ValueError('Invalid frustum dimensions')
    if not math.isfinite(zoom) or zoom <= 0:
        raise ValueError('Invalid camera zoom')
    effective = (*frustum[:2], frustum[2]/zoom, frustum[3]/zoom)
    view, screen = project_world_point(inverse, effective, target)
    if math.sqrt(sum(v*v for v in view)) < .01:
        raise ValueError('Aim target is at camera origin')
    yaw = math.atan2(view[0], -view[2])
    # Retail RsAiPlayer.h dfAIMDEV=1/3; normal Mario64 laser picks here.
    crosshair_pitch = math.atan((.5-1/3)*effective[3]/effective[0])
    pitch = math.atan2(view[1], math.hypot(view[0], view[2]))-crosshair_pitch
    return yaw, pitch, screen


def stick_toward(player, camera, target):
    dx, dz = target[0]-player[0], target[1]-player[1]
    distance = math.hypot(dx, dz)
    fx, fz = player[0]-camera[0], player[1]-camera[1]
    length = math.hypot(fx, fz)
    if not all(math.isfinite(x) for x in (*player, *camera, *target)) or length < .01:
        raise ValueError('Invalid player/camera coordinates')
    if distance < .01:
        return 0, 0, distance
    fx, fz = fx/length, fz/length
    return (round(32700*(-dx*fz+dz*fx)/distance),
            round(32700*(dx*fx+dz*fz)/distance), distance)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--command', type=Path, required=True)
    parser.add_argument('--offset', type=lambda x:int(x,0), required=True)
    parser.add_argument('--player', type=lambda x:int(x,0), required=True)
    parser.add_argument('--x', type=float)
    parser.add_argument('--z', type=float)
    parser.add_argument('--waypoints', type=Path,
                        help='Per-run JSON array of at most 64 normal stick destinations')
    parser.add_argument('--look-at', type=float, nargs=3,
                        help='Bounded normal right-stick aim at a world point; never writes camera memory')
    parser.add_argument('--hold-rt', type=float,
                        help='Hold only normal RT for 0.1..30 seconds; stops at dialogs; no hit/result inferred')
    parser.add_argument('--seconds', type=int, default=45, choices=range(1,91))
    parser.add_argument('--radius', type=float, default=2.0)
    parser.add_argument('--helicopter', action='store_true', help='Use normal flight sticks for an explicitly identified retail helicopter')
    parser.add_argument('--no-captures', action='store_true', help='Avoid GPU readbacks during timing runs; commands are bounded but have no screenshot acknowledgment')
    parser.add_argument('--jump', action='store_true', help='Use ordinary jump button during bounded walking pulses')
    args = parser.parse_args()
    executable, command = args.executable.resolve(), args.command.resolve()
    private_runtime = command.parent/'runtime'
    if (executable.parent != (ROOT/'build/mercenaries/bin/Release').resolve()
            and executable.parent != private_runtime):
        raise ValueError('Executable is not a workspace diagnostic')
    if not executable.name.startswith('mercenaries_recomp_run') or executable.suffix != '.exe':
        raise ValueError('Only explicitly named run diagnostics may receive test input')
    if command.name != 'gamepad-command.txt' or command.parent.parent != (ROOT/'artifacts/test-runs').resolve():
        raise ValueError('Command file must be an existing per-run gamepad protocol file')
    if not command.exists() or int((command.parent/'pid.txt').read_text().strip()) != args.pid:
        raise ValueError('Run PID and command file do not match')
    if args.hold_rt is not None:
        if args.look_at or args.waypoints or args.x is not None or args.z is not None:
            raise ValueError('Trigger hold cannot be combined with walking or aiming')
        trigger_renewal_ms(0,args.hold_rt)
        if args.seconds<args.hold_rt+1:
            raise ValueError('Overall timeout must allow the bounded trigger hold')
        targets=[]
    elif args.look_at:
        if args.waypoints or args.x is not None or args.z is not None:
            raise ValueError('Aim and walking destinations are mutually exclusive')
        if not all(math.isfinite(v) and abs(v) <= 10000 for v in args.look_at):
            raise ValueError('Invalid aim point')
        targets = []
    elif args.waypoints:
        route = args.waypoints.resolve(strict=True)
        if args.x is not None or args.z is not None or route.parent != command.parent or route.suffix != '.json':
            raise ValueError('Use either x/z or a JSON route in this exact diagnostic run directory')
        if route.stat().st_size > 16384:
            raise ValueError('Route file exceeds bounded size')
        targets = validate_waypoints(json.loads(route.read_text(encoding='utf-8')))
    else:
        targets = validate_waypoints([[args.x, args.z]])
    if not 0x10000 <= args.player <= 0x4000000-0xEC or not .25 <= args.radius <= 5:
        raise ValueError('Invalid actor/radius')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
    process = kernel.OpenProcess(0x1000|0x10,False,args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    last_text = command.read_text(encoding='utf-8').strip()
    sequence = int(last_text.split()[0])
    def read(address, size):
        if not 0 <= address <= 0x4000000-size:
            raise ValueError('Guest read out of bounds')
        output = ctypes.create_string_buffer(size); copied = ctypes.c_size_t()
        if not kernel.ReadProcessMemory(process,address+args.offset,output,size,ctypes.byref(copied)) or copied.value != size:
            raise ctypes.WinError(ctypes.get_last_error())
        return output.raw
    def u32(address): return struct.unpack('<I',read(address,4))[0]
    def f32(address): return struct.unpack('<f',read(address,4))[0]
    def issue(x=0,y=0,hold=1,rx=0,ry=0,analog=0):
        nonlocal last_text,sequence
        if command.read_text(encoding='utf-8').strip() != last_text:
            raise RuntimeError('Another controller changed this command file; yielding ownership')
        sequence += 1
        new_text = f'{sequence} 1 {hold} 0 {analog:X} {x} {y} {rx} {ry} {int(not args.no_captures)}'
        # Replace atomically in the same directory. This diagnostic must not
        # depend on a developer-specific Codex installation path, and a reader
        # must never observe a partially written controller command.
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=command.name + '.', suffix='.tmp', dir=command.parent
        )
        try:
            with os.fdopen(descriptor, 'w', encoding='utf-8', newline='') as temporary:
                temporary.write(new_text)
                temporary.flush()
                os.fsync(temporary.fileno())
            # The game's brief read can deny replacement on Windows. Keep the
            # command atomic and retry only sharing/access conflicts for 0.5 s.
            for attempt in range(51):
                try:
                    os.replace(temporary_name, command)
                    break
                except PermissionError as error:
                    if error.winerror not in (5, 32, 33) or attempt == 50:
                        raise
                    time.sleep(.01)
        except BaseException:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass
            raise
        last_text = new_text
    verified = False
    try:
        name = ctypes.create_unicode_buffer(32768); length = wintypes.DWORD(len(name))
        if not kernel.QueryFullProcessImageNameW(process,0,name,ctypes.byref(length)) or Path(name.value).resolve() != executable:
            raise RuntimeError('Diagnostic process image mismatch')
        verified = True
        started = time.monotonic(); anchor = None; anchor_time = started; target_index = 0
        while time.monotonic()-started < args.seconds:
            if ((not args.helicopter and (u32(args.player) != 0x2E32B8 or u32(args.player+4) != 0x660E4490)) or (args.helicopter and u32(args.player) != 0x2E21A8)):
                raise RuntimeError('Selected actor is no longer the player')
            state = u32(0x413F68)
            if state != 0xC2CBD863:
                print(f'Stopped at game substate {state:08X}',flush=True); break
            if args.hold_rt is not None:
                hold=trigger_renewal_ms(time.monotonic()-started,args.hold_rt)
                if hold==0:
                    print('Completed bounded normal RT hold; no hit or objective result inferred',flush=True); break
                issue(hold=hold,analog=0x80)
                time.sleep(min(.6,hold/1000))
                # The protocol captures only after pulse expiry+200ms. Waiting
                # for that here would release RT between renewals and prevent
                # the retail sustained laser lock. The final neutral command
                # provides the capture; inspect it/logs for actual results.
                continue
            player = f32(args.player+0xE0),f32(args.player+0xE8)
            camera_va = u32(0x414104)
            if args.look_at:
                if not 0x10000 <= camera_va <= 0x4000000-0xA0:
                    raise ValueError('Invalid camera pointer')
                inverse = struct.unpack('<16f',read(camera_va+0x50,64))
                frustum = struct.unpack('<4f',read(camera_va+0x90,16))
                zoom = f32(camera_va+0xB0)
                yaw,pitch,screen = aim_errors(inverse,frustum,args.look_at,zoom)
                print(f'aim yaw_error={math.degrees(yaw):.3f} pitch_error={math.degrees(pitch):.3f} screen={screen}',flush=True)
                if abs(yaw)<.012 and abs(pitch)<.012:
                    print('Reached aim tolerance through normal right-stick input',flush=True); break
                horizontal = abs(yaw) >= .012
                error = yaw if horizontal else pitch
                # Keep pulses above the game's nonlinear stick deadzone;
                # shorten their duration near the target instead of stalling
                # at a low analog magnitude.
                strength = 32700 if abs(error)>.07 else 25000
                strength = strength if error>0 else -strength
                hold = min(200,max(50,round(abs(error)*300)))
                issue(hold=hold,rx=strength if horizontal else 0,ry=0 if horizontal else strength)
                time.sleep(hold/1000+.35)
                deadline = time.monotonic()+3
                while not args.no_captures and not capture_acknowledged(command.parent,sequence) and time.monotonic()<deadline:
                    time.sleep(.1)
                if not args.no_captures and not capture_acknowledged(command.parent,sequence):
                    print('Stopped: aim command has no capture acknowledgment',flush=True); break
                continue
            camera = f32(camera_va+0x40),f32(camera_va+0x48)
            target = targets[target_index]
            x,y,distance = stick_toward(player,camera,target)
            print(f'pos={player} target={target} distance={distance:.3f}',flush=True)
            if distance <= args.radius:
                print(f'Reached waypoint {target_index+1}/{len(targets)}',flush=True)
                target_index += 1
                if target_index == len(targets): break
                anchor = None
                continue
            now = time.monotonic()
            if anchor is None or math.dist(player,anchor) > .5:
                anchor,anchor_time = player,now
            elif now-anchor_time > 7:
                print('Stopped: no movement; obstruction or stalled input/engine' if args.no_captures else 'Stopped at movement obstruction',flush=True); break
            # Two-second game-native pulse; expire to neutral even if this helper dies.
            # Shorten near the destination to avoid overshoot at retail run speed.
            hold = min(2000,max(100,round((distance-args.radius)/(45 if args.helicopter else 6)*1000)))
            issue(x,y,hold,analog=2 if args.jump else 0)
            time.sleep(hold/1000+.35)
            # A frame capture acknowledges consumption of this exact local
            # command. Run518 stopped accepting commands while stuck in an
            # audio worker; lack of motion alone mislabelled that as a wall.
            ack_deadline = time.monotonic() + 3.0
            while not args.no_captures and not capture_acknowledged(command.parent, sequence) and time.monotonic() < ack_deadline:
                time.sleep(.1)
            if not args.no_captures and not capture_acknowledged(command.parent, sequence):
                print(f'Stopped: command {sequence} has no capture acknowledgment; '
                      'possible engine/render stall, not confirmed collision',flush=True)
                break
        else:
            print('Bounded route timeout',flush=True)
    finally:
        try:
            if verified and command.read_text(encoding='utf-8').strip() == last_text:
                issue()
        finally:
            kernel.CloseHandle(process)


if __name__ == '__main__':
    main()
