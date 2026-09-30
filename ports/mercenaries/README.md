# Mercenaries ISO-to-port pipeline

This port supports the verified USA retail XBE with SHA-256
`AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7`.
Generated C, analysis output, extracted game data, and native build output are
disposable. Do not make correctness fixes directly in those directories.


## End-user first launch

The shipped entry point is `Mercenaries Recompiled.exe`. On first launch it
looks only for the completion marker beside its installed game data. If the
marker is present, it starts `mercenaries_recomp.exe` immediately. If the
marker is missing, it opens **Mercenaries Recompiled Setup**, asks the user to
select their legally obtained retail `.iso`, extracts it with the bundled
offline xdvdfs executable into a staging directory, verifies the complete
434-file inventory and retail XBE SHA-256, and atomically activates the
installation under `game_files/mercenaries-retail`. Later launches do not scan
or hash the 2.3 GiB asset tree.

The end-user distribution contains the launcher, internal game runtime,
`tools/xdvdfs.exe` plus its MIT license, and the hash-pinned SDL2 2.32.10
runtime plus its zlib license for optional DualSense input. It does not download tools and does
not require Python, CMake, Visual Studio, Xemu, or any
developer-machine path. Those build prerequisites below apply only when
rebuilding the port/toolchain from source.

## Rebuilding the port

Build prerequisites on a clean Windows computer:

- Python 3.12 available through the Windows `py` launcher
- Visual Studio 2022 with MSVC toolset 14.44.35207 and Windows SDK 10.0.26100.0
- PowerShell 7 or Windows PowerShell 5.1
- A legally obtained retail Mercenaries ISO owned by the user

From the repository root:

```powershell
& .\ports\mercenaries\scripts\Build-From-Iso.ps1 -IsoPath 'D:\Games\Mercenaries.iso'
```

The entry point verifies and installs the bundled xdvdfs 0.8.3 extractor,
creates the pinned Python environment, extracts
and verifies the retail XBE, regenerates and patches the disposable C tree,
builds the native executable, and writes `mercenaries-build-provenance.json`
beside it. The provenance file records the ISO, XBE, extractor, seed, patch,
generated-tree, exact toolchain-source archive, compiler/linker, CMake generator,
Windows SDK, and executable hashes needed to compare independent machines.
Two builds are equivalent only when their verified retail input and toolchain
identities match and their generated-tree and executable hashes agree.

The retail ISO is the only game-specific file supplied by the user. xdvdfs and
its MIT license are bundled under `third_party/xdvdfs`; setup verifies the
executable hash and performs no network download. Xemu is an optional engineering
reference and is never a build or runtime dependency. The supported reproducible native path pins Visual
Studio 2022, MSVC 14.44.35207, and Windows SDK 10.0.26100.0; no installation
path is hardcoded.