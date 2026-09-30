DSP56001 audio effect examples from Quinn Jensen's
[a56](http://www.zdomain.com/a56.html) assembler project, adapted for
DSP56300. The original code targeted a DSP56001 board with stereo SSI
audio I/O; hardware-specific initialization has been removed, leaving
just the per-sample effect routines.

Some of the code is based on stuff from the Motorola Dr. Bubb BBS and
Todd Day's archives.

## Usage

Assemble with `dsp56300-asm`, then stream s24le PCM through `dsp56300-emu`:

    cargo run -p dsp56300-asm -- examples/reverb.a56 -o reverb.out -f lod
    ffmpeg -f alsa -i default -f s24le -ac 2 -ar 48000 - |
      cargo run -p dsp56300-emu -- reverb.out |
      aplay -f S24_3LE -c 2 -r 48000

## Effects

| File | Description |
|------|-------------|
| reverb.a56 | 4-comb stereo reverb |
| sixcomb.a56 | 6-comb Moorer reverb (improved, requires m4 preprocessing) |
| chorus.a56 | Stereo chorus with delay modulation |
| flange.a56 | Stereo flanger |
| pink.a56 | Pink noise generator (1/f filtered LFSR) |
| caltone.a56 | Calibration tone via table lookup |
| thru.a56 | Passthrough (copy input to output) |

## Support files

| File | Description |
|------|-------------|
| tdsg.a56 | Runtime: I/O data areas, register save/restore |
| sinetab.inc | 256-entry sine table |

## License

    Copyright (C) 1990-1994 Quinn C. Jensen

    Permission to use, copy, modify, distribute, and sell this software
    and its documentation for any purpose is hereby granted without fee,
    provided that the above copyright notice appear in all copies and
    that both that copyright notice and this permission notice appear
    in supporting documentation.  The author makes no representations
    about the suitability of this software for any purpose.  It is
    provided "as is" without express or implied warranty.
