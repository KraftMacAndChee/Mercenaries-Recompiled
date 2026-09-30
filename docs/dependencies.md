# Dependencies and notices

This inventory describes the dependencies present in this source tree. Preserve
upstream file headers and accompanying notices when preparing a source release.
The root MIT license does not replace licenses on imported components.

| Component | Location | Recorded notice / provenance |
| --- | --- | --- |
| xboxrecomp toolkit | Shared runtime and translator | Root `LICENSE`, copyright sp00nz |
| xemu-derived APU and NV2A code | `src/apu`, `src/nv2a` | Per-file copyright and LGPL notices; verbatim upstream texts in `src/licenses` |
| DSP56300 emulator | `third_party/dsp56300/msvc-x64-v0.1.3` | `LICENSE` and `PROVENANCE.md`, including local patch hashes; patched source and Cargo.lock under `third_party/dsp56300/source-v0.1.3` |
| SDL2 2.32.10 | `ports/mercenaries/third_party/SDL2` | `LICENSE.txt` and `README.md` |
| xdvdfs 0.8.3 | `ports/mercenaries/third_party/xdvdfs` | `LICENSE` and `README.md` |
| Windows SDK shader compiler | `ports/mercenaries/third_party/d3dcompiler` | `README.txt`, SDK license and third-party notices |
| Hack Regular | `ports/mercenaries/resources/launcher` | `Hack-LICENSE.txt`, including underlying font notices |
| Earlier Anton font | Same launcher resource directory | `OFL.txt`; retained reference, not currently embedded |
| Lua 5.0.3 (host tests only) | `third_party/lua-5.0.3` | Unmodified public sources; `COPYRIGHT` (MIT), `UPSTREAM.json` with source hashes |
| Python build dependencies | `ports/mercenaries/requirements.txt` | Pinned Capstone, CMake and Ninja versions |

The player package explicitly includes notices for SDL2, xdvdfs, Hack, the
shader compiler, the root toolkit and DSP56300. The latter two are copied
verbatim into `licenses` by the build. The xemu LGPL/GPL texts are now staged with local builds and required by the
player packager. Their retrieval URLs and SHA-256 hashes are recorded under
`src/licenses`. This closes the missing-license-text gap; the exact source
revision/local-change and corresponding-source/relinking inventory still needs
review. Adding notices alone does not establish that distribution is license-complete.

The launcher embeds supplied artwork. Game data is extracted from the player's
ISO and is not included in the source archive or player package.

The source exporter includes the DSP56300 patched public source tree and locked
Rust dependency list, alongside the existing MSVC artifact. It excludes that
tree's `target` outputs and Git metadata. Rebuild with the toolchain and command
recorded in `PROVENANCE.md`; the existing library is not replaced by this
packaging correction. Transitive Cargo sources remain public package-manager
dependencies, not a claim of a completely offline vendored Rust build.

## Public compatibility source inventory

`src/licenses/xemu-source-inventory.json` inventories all 25 maintained APU/NV2A
C/H inputs except captured menu/font data. It pins comparison against public
xemu revision fc13b78060ffb3d64d435cbcec96c51bbfbf08ef, with local and comparison
file hashes. This revision is not asserted to be the historical import commit,
which was not recorded. Low line-match counts are comparison data, not evidence
of derivation. The complete current adapters, build files and notices are in
the source export; an xemu checkout is not needed.
Build and link them with the ordinary port CMake project; no opaque APU/NV2A
binary replaces that source. DSP rebuild instructions and local patch hashes
remain in its PROVENANCE.md. This closes the source-input inventory, without
claiming a legal opinion on the eventual distribution's licensing or granting
rights to supplied optional artwork/audio.
