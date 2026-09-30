param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$RunName,

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$BootMetadataRun,

    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$SeedSaveRun,

    [ValidatePattern('^mercenaries_recomp(?:_[A-Za-z0-9_-]+)?$')]
    [string]$CandidateName = 'mercenaries_recomp',

    [ValidateRange(-1, 5)]
    [int]$ResolutionScaleOverride = -1,

    [string]$OptionsFile,

    [switch]$ColdShaderCache,

    [switch]$AircraftOnly,

    [switch]$ManualController,

    [switch]$CaptureFlarePasses,

    [switch]$DisableFlareGrid,

    [switch]$CaptureTextureAlpha,

    [switch]$FullOpeningMovie,

    [switch]$TraceWeaponAudio,

    [switch]$TraceVehicleAudioState,

    [switch]$TraceVoiceEnergy,

    [switch]$TraceHqAudio,

    [switch]$TracePreparedCueLifetime,

    [switch]$TraceAudioMix,

    [switch]$TraceAudioMixAudit,

    [switch]$TraceCollisionAgent,

    [switch]$TraceVehicleDoor,

    [switch]$TraceAiStack,

    [switch]$CapturePcm,

    [switch]$EnableApuDsp,

    [switch]$TraceApuDspState,

    [switch]$DumpApuDspPram,

    [ValidateRange(1000, 100000000)]
    [int]$ApuDspCycleLimit = 2000000,

    [switch]$CaptureDisplay,

    [switch]$CaptureHumveeTransition,

    [ValidatePattern('^(?:0x)?[0-9A-Fa-f]{5,8}$')]
    [string]$ScanoutGuestDword,

    [ValidateRange(250, 60000)]
    [int]$CaptureDisplayIntervalMs = 10000,

    [ValidateRange(1, 512)]
    [int]$CaptureDisplayCount = 30,

    [switch]$DisableApuWorkers,

    [switch]$CombatProbe,

    [switch]$TwoClubs,

    [switch]$LoadSave,

    [switch]$LeanDiagnostics,

    [switch]$TraceCloudFlight,

    [switch]$TraceFullscreenImmediate,

    [ValidatePattern('^(?:0x)?[0-9A-Fa-f]{8}$')]
    [string]$TraceVshHlslHash,

    [ValidatePattern('^(?:0x)?[0-9A-Fa-f]{8}$')]
    [string]$TraceVshHlslRawHash,

    [ValidatePattern('^(?:0x)?[0-9A-Fa-f]{8}$')]
    [string]$TraceCombinerHlslHash,

    [ValidatePattern('^(?:0x)?[0-9A-Fa-f]{8}$')]
    [string]$DumpTextureOffset,

    [switch]$CaptureSatellitePasses,

    [switch]$PerformanceTrace,

    [switch]$TraceD3DDrawPerf,

    [switch]$TraceSkyState,

    [switch]$TraceSurfaceAliases,

    [ValidateRange(0, 600000)]
    [int]$TraceSurfaceTexturesAfterMs,

    [switch]$TraceShaderTiming,

    [switch]$TraceFileReads,

    [ValidateRange(20, 60000)]
    [int]$TraceGameStallMs = 0,

    [switch]$TraceHeapCensus,

    [switch]$TraceContiguousSurfaces,

    [switch]$TraceLuaPoscall,

    [switch]$TraceHashTable,

    [switch]$SpoolLogsLocally,

    [switch]$Wait
)

# Keep the required attribution channels together for performance runs.
if ($PerformanceTrace) {
    $LeanDiagnostics = $true
    $TraceD3DDrawPerf = $true
    $TraceShaderTiming = $true
    $TraceFileReads = $true
    $SpoolLogsLocally = $true
}

$ErrorActionPreference = 'Stop' 
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$executable = Join-Path $workspace ("build\mercenaries\bin\Release\{0}.exe" -f $CandidateName)
$gameDirectory = Join-Path $workspace 'game_files\mercenaries-retail'
$runDirectory = Join-Path $workspace (Join-Path 'artifacts\test-runs' $RunName)

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable not found: $executable"
}
if (-not (Test-Path -LiteralPath (Join-Path $gameDirectory 'default.xbe') -PathType Leaf)) {
    throw "Retail XBE not found: $gameDirectory\default.xbe"
}

if (Test-Path -LiteralPath $runDirectory) {
    throw "Use a new diagnostic run directory: $runDirectory"
}
New-Item -ItemType Directory -Path $runDirectory | Out-Null
if ($OptionsFile) {
    $optionsSource = Get-Item -LiteralPath $OptionsFile -ErrorAction Stop
    if ($optionsSource.PSIsContainer -or $optionsSource.Extension -ne '.ini' -or
        ($optionsSource.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'OptionsFile must be a regular INI file'
    }
    $runtimeDirectory = Join-Path $runDirectory 'runtime'
    New-Item -ItemType Directory -Path $runtimeDirectory | Out-Null
    Copy-Item -LiteralPath $executable -Destination $runtimeDirectory
    Copy-Item -LiteralPath (Join-Path (Split-Path $executable) 'SDL2.dll') -Destination $runtimeDirectory
    Copy-Item -LiteralPath $optionsSource.FullName -Destination (Join-Path $runtimeDirectory 'mercenaries_recomp.ini')
    $executable = Join-Path $runtimeDirectory "$CandidateName.exe"
}


if ($BootMetadataRun) {
    # Reuse only private system/cache initialization from a named diagnostic.
    # No UDATA, TDATA, profiles, checkpoints, or user preview saves are copied.
    $sourceRoot = Join-Path $workspace "artifacts\test-runs\$BootMetadataRun\saves"
    $saveRoot = Join-Path $runDirectory 'saves'
    New-Item -ItemType Directory -Force -Path $saveRoot | Out-Null
    foreach ($directory in @('System', 'Cache')) {
        $source = Join-Path $sourceRoot $directory
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            if ($directory -eq 'System') {
                throw "Missing private boot metadata: $source"
            }
            continue
        }
        $items = @((Get-Item -LiteralPath $source)) +
            @(Get-ChildItem -LiteralPath $source -Recurse -Force)
        if ($items | Where-Object {
                $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
            throw "Private boot metadata contains a link: $source"
        }
        Copy-Item -LiteralPath $source -Destination $saveRoot -Recurse -Force
    }
}

if ($SeedSaveRun) {
    # Diagnostic-only save seeding. Copy the retail-created title/user save
    # data into this run's private save root before launch; never share a live
    # save directory between processes and never modify the source artifact.
    $sourceRoot = Join-Path $workspace "artifacts\test-runs\$SeedSaveRun\saves"
    $saveRoot = Join-Path $runDirectory 'saves'
    New-Item -ItemType Directory -Force -Path $saveRoot | Out-Null
    foreach ($directory in @('UDATA', 'TDATA', 'TitleData', 'UserData')) {
        $source = Join-Path $sourceRoot $directory
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            continue
        }
        $items = @((Get-Item -LiteralPath $source)) +
            @(Get-ChildItem -LiteralPath $source -Recurse -Force)
        if ($items | Where-Object {
                $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
            throw "Private save seed contains a link: $source"
        }
        Copy-Item -LiteralPath $source -Destination $saveRoot -Recurse -Force
    }
    $retailSave = Join-Path $saveRoot 'UserData\9AA9F19E10CF\Mercenaries Saves'
    if (-not (Test-Path -LiteralPath $retailSave -PathType Leaf)) {
        throw "Seed run does not contain a retail Mercenaries save: $SeedSaveRun"
    }
}

$logDirectory = $runDirectory
if ($SpoolLogsLocally) {
    # Synchronous stderr flushes can block the retail main thread when a long,
    # trace-heavy run writes directly into a synchronized OneDrive workspace.
    # Spool only stdout/stderr on the local system drive; captures and all game
    # inputs remain in the normal run directory.  -Wait copies the completed
    # logs back into the artifact folder for permanent analysis.
    $logDirectory = Join-Path ([IO.Path]::GetTempPath()) `
        (Join-Path 'mercenaries-recomp-runs' $RunName)
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    Set-Content -LiteralPath (Join-Path $runDirectory 'log-spool-path.txt') `
        -Value $logDirectory
}

$settings = [ordered]@{
    MERCENARIES_HIDE_WINDOW = '1'
    MERCENARIES_TEST_ISOLATE_INPUT = '1'
    MERCENARIES_TEST_SAVE_DIR = (Join-Path $runDirectory 'saves')
    MERCENARIES_TEST_MUTE_HOST_AUDIO = '1'
    MERCENARIES_DIAGNOSTIC_BUFFERED_LOGS = '1'
    MERCENARIES_TEST_EXACT_NEW_GAME_SEQUENCE = '1'
    # Use the actual input-layer option names and preserve its established
    # defaults; the former NEW_GAME_* spellings were silently ignored.
    MERCENARIES_TEST_EXACT_START_DELAY_MS = '45000'
    MERCENARIES_TEST_EXACT_A1_DELAY_MS = '65000'
    MERCENARIES_TEST_EXACT_A2_DELAY_MS = '70000'
    MERCENARIES_TEST_EXACT_A3_DELAY_MS = '75000'
    MERCENARIES_TEST_EXACT_PULSE_MS = '300'
    MERCENARIES_TRACE_TEST_INPUT = '1'
    MERCENARIES_TEST_SKIP_MOVIE_AFTER_POLLS = '30'
    MERCENARIES_TEST_AUTO_Y = '1'
    MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY = '1'
    MERCENARIES_TEST_AUTO_Y_ONCE = '1'
    MERCENARIES_TEST_AUTO_Y_DELAY_MS = '600'
    MERCENARIES_TEST_AUTO_Y_PULSE_MS = '500'
    MERCENARIES_TEST_AUTO_Y_STOP_AFTER_STANDUP = '1'
    MERCENARIES_TRACE_SCRIPT_USE_AFTER_MOVIE = '1'
    MERCENARIES_TEST_AUTO_AIRCRAFT_PICKUP_ROUTE = '1'
    MERCENARIES_TEST_AUTO_FIRE_AFTER_AIRCRAFT_RIFLE = '1'
    MERCENARIES_TEST_AUTO_DRIVE_GATE_ROUTE = '1'
    MERCENARIES_TEST_AUTO_ENTER_ALLIED_HQ = '1'
    MERCENARIES_TEST_AUTO_DRIVE_AFTER_AIRCRAFT_DELAY_MS = '45000'
    MERCENARIES_TRACE_VEHICLE_REWARD = '1'
    MERCENARIES_TRACE_ROADBLOCK_ACTORS = '1'
    MERCENARIES_TRACE_XACT_FAILURES = '1'
    MERCENARIES_TRACE_XACT_INTERNAL_FAILURES = '1'
    MERCENARIES_TRACE_NOTIFICATION_LIST = '1'
    # Low-volume timing summaries distinguish guest simulation stalls from
    # flip pacing, DXGI Present, and long gaps before the renderer is entered.
    MERCENARIES_TRACE_PRESENT_TIMING = '1'
    MERCENARIES_DUMP_CRASH_GUEST_PATH = (Join-Path $runDirectory 'crash-guest.bin')
    MERCENARIES_CAPTURE_GATE_ROUTE_STUCK_PATH = (Join-Path $runDirectory 'route-stuck.bmp')
    MERCENARIES_CAPTURE_AIRCRAFT_ROUTE_PATH = (Join-Path $runDirectory 'aircraft-interactions-complete.bmp')
    MERCENARIES_CAPTURE_AIRCRAFT_ROUTE_ABORT_PATH = (Join-Path $runDirectory 'aircraft-humvee-timeout.bmp')
    MERCENARIES_CAPTURE_AUTHORED_NORTH_ROUTE_PATH = (Join-Path $runDirectory 'route-finished.bmp')
    MERCENARIES_CAPTURE_ALLIED_HQ_SETTLED_PATH = (Join-Path $runDirectory 'allied-hq-settled.bmp')
    MERCENARIES_CAPTURE_ALLIED_HQ_WALK_ABORT_PATH = (Join-Path $runDirectory 'allied-hq-walk-timeout.bmp')
    MERCENARIES_CAPTURE_ALLIED_HQ_APPROACH_PATH = (Join-Path $runDirectory 'allied-hq-approach.bmp')
    MERCENARIES_CAPTURE_ALLIED_HQ_INTERIOR_PATH = (Join-Path $runDirectory 'allied-hq-interior.bmp')
    MERCENARIES_CAPTURE_ALLIED_HQ_BRIEFING_PATH = (Join-Path $runDirectory 'allied-hq-briefing.bmp')
    MERCENARIES_CAPTURE_ALLIED_MISSION_ACCEPTED_PATH = (Join-Path $runDirectory 'allied-mission-accepted.bmp')
    MERCENARIES_CAPTURE_TWO_CLUBS_SUBDUE_PATH = (Join-Path $runDirectory 'two-clubs-subdue-probe.bmp')
    MERCENARIES_CAPTURE_TWO_CLUBS_ABORT_PATH = (Join-Path $runDirectory 'two-clubs-abort.bmp')
    MERCENARIES_CAPTURE_TWO_CLUBS_SUPPORT_PATH = (Join-Path $runDirectory 'two-clubs-support.bmp')
}

if ($ResolutionScaleOverride -ge 0) {
    # Diagnostic-only override: isolate route behavior from the developer's
    # persisted Recomp Options without rewriting mercenaries_recomp.ini.
    $settings.MERCENARIES_TEST_RESOLUTION_SCALE =
        [string]$ResolutionScaleOverride
}

if ($PerformanceTrace) {
    $settings.MERCENARIES_TRACE_TEXTURE_PERF = '1'
}

if ($TraceD3DDrawPerf) {
    $settings.MERCENARIES_TRACE_D3D_DRAW_PERF = '1'
}

if ($TraceSkyState) {
    $settings.MERCENARIES_TRACE_SKY_STATE = '1'
    $settings.MERCENARIES_TRACE_COMBINER = '1'
}

if ($TraceSurfaceAliases) {
    $settings.MERCENARIES_TRACE_SURFACE_ALIASES = '1'
    $settings.MERCENARIES_TRACE_SURFACE_TEXTURES = '1'
    $settings.MERCENARIES_TRACE_SURFACE_TEXTURE_REJECTS = '1'
}

if ($PSBoundParameters.ContainsKey('TraceSurfaceTexturesAfterMs')) {
    $settings.MERCENARIES_TRACE_SURFACE_TEXTURES = '1'
    $settings.MERCENARIES_TRACE_SURFACE_TEXTURE_REJECTS = '1'
    $settings.MERCENARIES_TRACE_SURFACE_METHODS = '1'
    $settings.MERCENARIES_TRACE_SURFACE_DELAY_MS =
        [string]$TraceSurfaceTexturesAfterMs
}

if ($TraceShaderTiming) {
    $settings.MERCENARIES_TRACE_SHADER_TIMING = '1'
}

if ($TraceFileReads) {
    $settings.MERCENARIES_TRACE_FILE_READS = '1'
}

if ($TraceGameStallMs -gt 0) {
    $settings.MERCENARIES_TRACE_GAME_STALL_MS =
        $TraceGameStallMs.ToString([Globalization.CultureInfo]::InvariantCulture)
}

if ($TraceHeapCensus) {
    # Emit one bounded live-allocation census only if the guest heap reaches
    # OOM. This is intentionally separate from the per-allocation heap trace,
    # whose synchronous volume would perturb the frame-pacing investigation.
    $settings.MERCENARIES_TRACE_HEAP_CENSUS = '1'
}

if ($TraceContiguousSurfaces) {
    # Trace only color/depth-sized retail contiguous allocations and frees.
    # The bounded ring preserves caller context without a full heap log.
    $settings.MERCENARIES_TRACE_CONTIG_SURFACES = '1'
}

if ($TraceLuaPoscall) {
    # Records the bounded ring of luaD_poscall callers and dumps it on a
    # crash. Also records the callback stack contract immediately before the
    # poscall path. These diagnostics do not alter guest state.
    $settings.MERCENARIES_TRACE_LUA_POSCALL_SITES = '1'
    $settings.MERCENARIES_TRACE_LUA_CALLFRAMES = '1'
}

if ($TraceHashTable) {
    $settings.MERCENARIES_TRACE_HASH_TABLE = '1'
}

if ($LeanDiagnostics) {
    # Focused renderer/audio captures should not inherit the high-volume
    # reward, roadblock-actor and notification-list observers used by the
    # broad gameplay route. Those observers are useful independently, but can
    # materially change frame pacing and vehicle navigation during a targeted
    # A/B run.
    $settings.Remove('MERCENARIES_TRACE_VEHICLE_REWARD')
    $settings.Remove('MERCENARIES_TRACE_ROADBLOCK_ACTORS')
    $settings.Remove('MERCENARIES_TRACE_NOTIFICATION_LIST')
    $settings.Remove('MERCENARIES_TRACE_XACT_FAILURES')
    $settings.Remove('MERCENARIES_TRACE_XACT_INTERNAL_FAILURES')
}

if ($TraceCloudFlight) {
    # The authored C-17 exterior uses the CloudFlight/RushingAir transparent
    # strip path. Keep this opt-in because it records every vertex from a
    # bounded set of matching draws.
    $settings.MERCENARIES_TRACE_CLOUD_FLIGHT = '1'
    # The exterior C-17 shot occurs before the normal New Game input sequence,
    # so the exact CloudFlight predicate must be armed from process start.
    $settings.MERCENARIES_CAPTURE_ARRAYS_AFTER_MS = '0'
}

if ($FullOpeningMovie) {
    # Leave the three normal New Game/agent/Accept presses unchanged, then let
    # the retail movie end naturally. No controller pulses occur inside it.
    $settings.Remove('MERCENARIES_TEST_SKIP_MOVIE_AFTER_POLLS')
    [Environment]::SetEnvironmentVariable(
        'MERCENARIES_TEST_SKIP_MOVIE_AFTER_POLLS', $null, 'Process')
}

if ($AircraftOnly) {
    $settings.Remove('MERCENARIES_TEST_AUTO_DRIVE_GATE_ROUTE')
    $settings.Remove('MERCENARIES_TEST_AUTO_ENTER_ALLIED_HQ')
}

if ($LoadSave) {
    # Diagnostic-only fresh-process reload. The caller seeds this run's
    # private UserData directory; only normal retail menu input is emitted.
    $settings.Remove('MERCENARIES_TEST_EXACT_NEW_GAME_SEQUENCE')
    $settings.Remove('MERCENARIES_TEST_EXACT_START_DELAY_MS')
    $settings.Remove('MERCENARIES_TEST_EXACT_A1_DELAY_MS')
    $settings.Remove('MERCENARIES_TEST_EXACT_A2_DELAY_MS')
    $settings.Remove('MERCENARIES_TEST_EXACT_A3_DELAY_MS')
    $settings.Remove('MERCENARIES_TEST_EXACT_PULSE_MS')
    $settings.Remove('MERCENARIES_TEST_SKIP_MOVIE_AFTER_POLLS')
    $settings.Remove('MERCENARIES_TEST_AUTO_Y')
    $settings.Remove('MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY')
    $settings.Remove('MERCENARIES_TEST_AUTO_Y_ONCE')
    $settings.Remove('MERCENARIES_TEST_AUTO_Y_DELAY_MS')
    $settings.Remove('MERCENARIES_TEST_AUTO_Y_PULSE_MS')
    $settings.Remove('MERCENARIES_TEST_AUTO_Y_STOP_AFTER_STANDUP')
    $settings.Remove('MERCENARIES_TRACE_SCRIPT_USE_AFTER_MOVIE')
    $settings.Remove('MERCENARIES_TEST_AUTO_AIRCRAFT_PICKUP_ROUTE')
    $settings.Remove('MERCENARIES_TEST_AUTO_FIRE_AFTER_AIRCRAFT_RIFLE')
    $settings.Remove('MERCENARIES_TEST_AUTO_DRIVE_GATE_ROUTE')
    $settings.Remove('MERCENARIES_TEST_AUTO_ENTER_ALLIED_HQ')
    $settings.MERCENARIES_TEST_EXACT_LOAD_GAME_SEQUENCE = '1'
    $settings.MERCENARIES_TEST_EXACT_LOAD_SINGLE_POLL = '1'
    # Load/Save/Options all share the same retail modal stack and front-end
    # input dispatcher. Keep the low-volume transition checkpoints enabled
    # for this focused route so a menu that closes itself can be attributed to
    # the exact event/state transition instead of inferred from a screenshot.
    $settings.MERCENARIES_TRACE_INPUT = '1'
    $settings.MERCENARIES_CAPTURE_GAME_PATH =
        (Join-Path $runDirectory 'loaded-save.bmp')
    $settings.MERCENARIES_CAPTURE_GAME_DELAY_MS = '5000'
    # Keep a local, sequence-acknowledged controller/capture channel available
    # for save-menu diagnostics. Retail boot timing varies enough that a fixed
    # pulse schedule alone cannot reliably distinguish title/menu/modal state.
    $settings.MERCENARIES_TEST_GAMEPAD_FILE =
        (Join-Path $runDirectory 'gamepad-command.txt')
    $settings.MERCENARIES_CAPTURE_TEST_GAMEPAD_PREFIX =
        (Join-Path $runDirectory 'gamepad')
}

if ($TwoClubs -and -not $AircraftOnly) {
    $settings.MERCENARIES_TEST_AUTO_TWO_CLUBS = '1'
    # Optional numeric test data: sequence stage world-x world-z. No file is
    # created by default; absent data keeps the default route intact.
    $settings.MERCENARIES_TEST_HQ_WAYPOINT_FILE =
        (Join-Path $runDirectory 'hq-waypoints.txt')
    $settings.MERCENARIES_CAPTURE_HQ_WAYPOINT_PREFIX =
        (Join-Path $runDirectory 'waypoint-override')
    $settings.MERCENARIES_TEST_GAMEPAD_FILE =
        (Join-Path $runDirectory 'gamepad-command.txt')
    $settings.MERCENARIES_CAPTURE_TEST_GAMEPAD_PREFIX =
        (Join-Path $runDirectory 'gamepad')
}

if ($TraceFullscreenImmediate) {
    # Generic renderer-state capture for screen-covering immediate draws. The
    # existing support-menu screenshot is the gate, so the long route does not
    # consume the bounded trace budget on boot/front-end quads.
    $settings.MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE = '1'
    $settings.MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE_GATE_FILE =
        (Join-Path $runDirectory 'two-clubs-support.bmp')
    $settings.MERCENARIES_TEST_SURVIVOR_FLASH_FILE =
        (Join-Path $runDirectory 'survivor-flash.trigger')
    $settings.MERCENARIES_CAPTURE_COLORED_OVERLAY_PREFIX =
        (Join-Path $runDirectory 'colored-overlay')
    $settings.MERCENARIES_TRACE_SCREEN_FLASH = '1'
}

if ($TraceVshHlslHash) {
    $settings.MERCENARIES_TRACE_VSH_HLSL_HASH =
        ($TraceVshHlslHash -replace '^0x', '')
}

if ($TraceVshHlslRawHash) {
    $settings.MERCENARIES_TRACE_VSH_HLSL_RAW_HASH =
        ($TraceVshHlslRawHash -replace '^0x', '')
}

if ($TraceCombinerHlslHash) {
    $settings.MERCENARIES_TRACE_COMBINER_HLSL_HASH =
        ($TraceCombinerHlslHash -replace '^0x', '')
}

if ($DumpTextureOffset) {
    # Opt-in guest-texture evidence. The renderer filters by the exact VRAM
    # allocation so long gameplay routes do not accumulate unrelated dumps.
    $settings.MERCENARIES_DUMP_TEXTURE_PREFIX =
        (Join-Path $runDirectory 'guest-texture')
    $settings.MERCENARIES_DUMP_TEXTURE_OFFSET = $DumpTextureOffset
}

if ($CaptureSatellitePasses) {
    $settings.MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX =
        (Join-Path $runDirectory 'satellite-pass')
    $settings.MERCENARIES_CAPTURE_SATELLITE_PASS_GATE_FILE =
        (Join-Path $runDirectory 'satellite-pass.ready')
}

if ($TraceWeaponAudio) {
    $settings.MERCENARIES_TRACE_PLAYER_FIRE_ANIM = '1'
    $settings.MERCENARIES_TRACE_WEAPON_FIRE_SOUND = '1'
    $settings.MERCENARIES_TRACE_XACT_PLAY = '1'
    $settings.MERCENARIES_TRACE_XACT_RETIRE = '1'
    $settings.MERCENARIES_TRACE_XACT_CUES_AFTER_MOVIE = '1'
    $settings.MERCENARIES_TRACE_XACT_VOICE_START = '1'
    $settings.MERCENARIES_TRACE_XACT_SOUND_UPDATE = '1'
    $settings.MERCENARIES_TRACE_XACT_PROPERTIES = '1'
    $settings.MERCENARIES_TRACE_XACT_ALLOC_SUMMARY = '1'
    # This records the actual NV2A APU VOICE_ON method, after XACT allocation
    # and DirectSound translation. It closes the current diagnostic gap where
    # a retail cue can be accepted while no hardware voice ever starts.
    $settings.MERCENARIES_TRACE_APU_VOICE_ON = '1'
    # Once-per-second APU summaries are sufficient to distinguish a missing
    # retail cue from an active voice that is being routed or attenuated away.
    # Avoid the per-frame DSP trace here because it materially perturbs the
    # performance/audio behavior under investigation.
    $settings.MERCENARIES_TRACE_APU_PERF = '1'
    $settings.MERCENARIES_TRACE_XAUDIO_QUEUE = '1'
}

if ($TraceVehicleAudioState) {
    # Read-only title-state observer for RsSoundEffectCar2::Update. Pair the
    # authored state snapshot with accepted XACT cue requests, without the
    # high-volume per-voice instrumentation used by -TraceWeaponAudio.
    $settings.MERCENARIES_TRACE_VEHICLE_AUDIO_STATE = '1'
    $settings.MERCENARIES_TRACE_XACT_PLAY = '1'
    $settings.MERCENARIES_TRACE_XACT_CUES_AFTER_MOVIE = '1'
}

# Same opt-in observer set as the manual harness; no shipping mix changes.
if ($TraceAudioMixAudit) {
    $settings.MERCENARIES_TRACE_APU_PITCH = '1'
    $settings.MERCENARIES_TRACE_APU_VOICE_ON = '1'
    $settings.MERCENARIES_TRACE_APU_VOICE_ENERGY = '1'
    $settings.MERCENARIES_TRACE_XACT_CUES = '1'
    $settings.MERCENARIES_TRACE_XACT_BANKS = '1'
}

if ($TraceAudioMix) {
    $settings.MERCENARIES_TRACE_APU_PCM = '1'
    $settings.MERCENARIES_TRACE_APU_MIXBINS = '1'
}

if ($TraceCollisionAgent) {
    $settings.MERCENARIES_TRACE_COLLISION_AGENT = '1'
}

if ($TraceVehicleDoor) {
    $settings.MERCENARIES_TRACE_VEHICLE_DOOR = '1'
}

if ($ScanoutGuestDword) {
    # Generic read-only guest-memory correlation sampled at presentation.
    # This keeps title-state attribution out of the renderer and lets the
    # same deterministic route compare any candidate DWORD/flag.
    $settings.MERCENARIES_TRACE_SCANOUT_SURFACE = '1'
    $settings.MERCENARIES_TRACE_SCANOUT_GUEST_DWORD = $ScanoutGuestDword
}

if ($CapturePcm) {
    # XAudio writes stereo signed-16-bit, 48 kHz PCM before host mute is
    # applied, so unattended comparisons remain silent without losing data.
    $settings.MERCENARIES_DUMP_XAUDIO_PCM_PATH =
        (Join-Path $runDirectory 'host-output-s16le-48k-stereo.pcm')
}

if ($EnableApuDsp) {
    # Fidelity diagnostic: execute the title-provided GP/EP programs through
    # the bundled DSP56300 core. Keep this explicit until the authored path
    # completes both processors without falling back to the VP monitor.
    $settings.MERCENARIES_ENABLE_APU_DSP = '1'
    $settings.MERCENARIES_TEST_APU_DSP_CYCLE_LIMIT =
        [string]$ApuDspCycleLimit
}

if ($TraceApuDspState) {
    if (-not $EnableApuDsp) {
        throw 'DSP state tracing requires -EnableApuDsp'
    }
    # The runtime single-steps the first EP frame and records the exact
    # instruction that changes the suspect loop-control word. This is an
    # opt-in fidelity diagnostic and never affects normal/player launches.
    $settings.MERCENARIES_TRACE_APU_DSP_N5 = '1'
    $settings.MERCENARIES_DUMP_APU_DSP_STATE_PATH =
        (Join-Path $runDirectory 'ep-preloop-state.bin')
    $settings.MERCENARIES_DUMP_APU_DSP_FAILURE_STATE_PATH =
        (Join-Path $runDirectory 'ep-failure-state.bin')
}

if ($DumpApuDspPram) {
    if (-not $EnableApuDsp) {
        throw 'DSP PRAM capture requires -EnableApuDsp'
    }
    $settings.MERCENARIES_DUMP_APU_DSP_PRAM = '1'
}

if ($CaptureDisplay) {
    $settings.MERCENARIES_CAPTURE_DISPLAY_PREFIX =
        (Join-Path $runDirectory 'display')
    $settings.MERCENARIES_CAPTURE_DISPLAY_INTERVAL_MS =
        [string]$CaptureDisplayIntervalMs
    $settings.MERCENARIES_CAPTURE_DISPLAY_COUNT =
        [string]$CaptureDisplayCount
}

if ($CaptureHumveeTransition) {
    $settings.MERCENARIES_CAPTURE_HUMVEE_TRANSITION_PREFIX =
        (Join-Path $runDirectory 'humvee-transition')
}
if ($DisableApuWorkers) {
    # A/B diagnostic against Xemu's serial VP processing. This is never set in
    # normal previews; it isolates worker scheduling from cue/routing faults.
    $settings.MERCENARIES_DISABLE_APU_WORKERS = '1'
}

if ($TraceVoiceEnergy) {
    # This walks every active voice and is intentionally opt-in: it can slow
    # the APU enough to create diagnostic-only XAudio underruns.
    $settings.MERCENARIES_TRACE_APU_VOICE_ENERGY = '1'
}

if ($TracePreparedCueLifetime) {
    $settings.MERCENARIES_TRACE_PREPARED_CUE_LIFETIME = '1'
}
if ($TraceHqAudio) {
    # The title-side observer activates only while the Allied HQ briefing flag
    # is set, so cue diagnostics stay bounded to the dialogue under study.
    $settings.MERCENARIES_TRACE_HQ_AUDIO = '1'
}

if ($TraceAiStack) {
    # Existing mismatch-only observer: no register repair or skipped calls.
    $settings.MERCENARIES_TRACE_AI_UPDATE_STACK = '1'
}

if ($CombatProbe) {
    $settings.MERCENARIES_TEST_AUTO_ROADBLOCK_ON_FOOT = '1'
    $settings.MERCENARIES_TEST_AUTO_FIRE_AFTER_ROADBLOCK = '1'
    $settings.MERCENARIES_TRACE_BULLET_HITS = '1'
    $settings.MERCENARIES_TRACE_PLAYER_FIRE_ANIM = '1'
    $settings.MERCENARIES_TRACE_WEAPON_FIRE_SOUND = '1'
    $settings.MERCENARIES_CAPTURE_ROADBLOCK_EXIT_PATH =
        (Join-Path $runDirectory 'roadblock-exit.bmp')
    $settings.MERCENARIES_CAPTURE_ROADBLOCK_BYPASS_PATH =
        (Join-Path $runDirectory 'roadblock-bypass.bmp')
}

if ($ManualController) {
    $settings.MERCENARIES_TEST_GAMEPAD_FILE = (Join-Path $runDirectory 'gamepad-command.txt')
    $settings.MERCENARIES_TEST_SPAWN_FILE = (Join-Path $runDirectory 'spawn-command.txt')
    $settings.MERCENARIES_CAPTURE_TEST_GAMEPAD_PREFIX = (Join-Path $runDirectory 'gamepad')
    Set-Content -LiteralPath (Join-Path $runDirectory 'log-spool-path.txt') -Value $logDirectory
}
if ($DisableFlareGrid) { $settings.MERCENARIES_DISABLE_FLARE_GRID = '1' }
if ($CaptureTextureAlpha) { $settings.MERCENARIES_CAPTURE_TEXTURE_ALPHA = '1' }
if ($CaptureFlarePasses) {
    $settings.MERCENARIES_CAPTURE_FLARE_COMPOSITE_PREFIX = (Join-Path $runDirectory 'flare-composite')
    $settings.MERCENARIES_CAPTURE_FLARE_COMPOSITE_GATE_FILE = (Join-Path $runDirectory 'flare-capture.gate')
    $settings.MERCENARIES_CAPTURE_FLARE_PROBE_PREFIX = (Join-Path $runDirectory 'flare-probe')
    $settings.MERCENARIES_CAPTURE_FLARE_PROBE_GATE_FILE = (Join-Path $runDirectory 'flare-capture.gate')
    $settings.MERCENARIES_TRACE_SUSPICIOUS_FLARE_BOUNDS = '8'
    $settings.MERCENARIES_TRACE_SUSPICIOUS_FLARE_GATE_FILE = (Join-Path $runDirectory 'flare-capture.gate')
    $settings.MERCENARIES_CAPTURE_SUSPICIOUS_FLARE_PREFIX = (Join-Path $runDirectory 'flare-suspicious')
}

# A file-backed controller channel must start with a complete neutral command.
# Creating the path only in the environment left interactive diagnostic runs
# unable to issue their first command until an operator initialized the file.
if ($settings.Contains('MERCENARIES_TEST_GAMEPAD_FILE')) {
    Set-Content -LiteralPath $settings.MERCENARIES_TEST_GAMEPAD_FILE `
        -Value '0 1 1 0 0 0 0 0 0 0'
}

$settings.MERCENARIES_PREVIEW_LOG_DIR = Join-Path $runDirectory 'preview-logs'
if ($ColdShaderCache) {
    $settings.LOCALAPPDATA = Join-Path $runDirectory 'local-appdata'
    New-Item -ItemType Directory -Path $settings.LOCALAPPDATA | Out-Null
}
$settings | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'diagnostic-settings.json')
foreach ($entry in $settings.GetEnumerator()) {
    [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
}

# Start-Process joins ArgumentList elements without preserving spaces. Passing
# the retail directory as one explicitly quoted token prevents it from being
# truncated to a sibling "Mercenaries" path.
$quotedGameDirectory = '"' + $gameDirectory + '"'
$process = Start-Process -FilePath $executable `
    -ArgumentList $quotedGameDirectory `
    -WorkingDirectory $workspace `
    -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $logDirectory 'stdout.log') `
    -RedirectStandardError (Join-Path $logDirectory 'stderr.log') `
    -PassThru

Set-Content -LiteralPath (Join-Path $runDirectory 'pid.txt') -Value $process.Id
$result = [pscustomobject]@{
    ProcessId = $process.Id
    RunDirectory = $runDirectory
    RetailXbe = (Join-Path $gameDirectory 'default.xbe')
}
$result

if ($Wait) {
    $process.WaitForExit()
    if ($SpoolLogsLocally) {
        Copy-Item -LiteralPath (Join-Path $logDirectory 'stdout.log') `
            -Destination (Join-Path $runDirectory 'stdout.log') -Force
        Copy-Item -LiteralPath (Join-Path $logDirectory 'stderr.log') `
            -Destination (Join-Path $runDirectory 'stderr.log') -Force
    }
    Set-Content -LiteralPath (Join-Path $runDirectory 'exit-code.txt') `
        -Value ('0x{0:X8} ({0})' -f $process.ExitCode)
    [pscustomobject]@{
        ProcessId = $process.Id
        ExitCode = $process.ExitCode
        RunDirectory = $runDirectory
    }
}
