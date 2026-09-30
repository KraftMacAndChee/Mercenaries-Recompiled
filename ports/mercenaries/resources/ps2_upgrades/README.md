# Optional PS2 assets

Assets supplied by the project maintainer for the PS2 Upgrades option. Original
filenames and file hashes are recorded in manifest.json. The BGRA file is a
lossless 256 x 256, row-major BGRA8 conversion of Allies_avehicle_M1A2.png; alpha
is 255 when the source has no alpha channel.

Player Dragunov firing adds rifle_shot1.wav as its own delayed layer, alongside
the original randomized attack, AK47 and quieter cannon layers, using PS2 pitch,
volume, timing and attenuation settings.
Player 20 mm cannon firing uses wpn_cannon_20mm_fire.wav.
The retail autocannon release wave (wpn_cannon_20mm_fire_nd_01) is retained.
The supplied fire_01 and fire_st files are retained only as reference assets.
They are not played. The autocannon uses the PS2's
finite per-shot cue instead of looping the replacement inside the Xbox cue.

Runtime replacements are embedded resources. The option defaults off and never
rewrites installed game assets. Only the player high-detail sound banks match;
NPC, reload and other weapon events retain their original data. Active buffers
remain alive until playback finishes when the option is changed.
