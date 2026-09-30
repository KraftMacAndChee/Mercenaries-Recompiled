# Test data

## sixcomb.lod

Assembled LOD output of `examples/sixcomb.a56`, a stereo reverb effect
by Quinn Jensen. Originally written for the DSP56001, adapted for the
DSP56300. The source uses m4 macros and was preprocessed with
`m4 sixcomb.a56 | dsp56300-asm -f lod -o sixcomb.lod`.

Used by the `sixcomb` differential tests to compare interpreter vs
block JIT execution on a real DSP program.
