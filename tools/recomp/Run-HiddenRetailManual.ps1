param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$RunName,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^mercenaries_recomp_run[A-Za-z0-9_-]+$')]
    [string]$CandidateName,
    [ValidateRange(-1, 5)]
    [int]$ResolutionScaleOverride = -1,
    [string]$OptionsFile,
    [string]$TestGameDirectory,
    [ValidateRange(0, 600000)]
    [int]$TraceSurfaceTexturesAfterMs,
    [switch]$TraceFullscreenImmediateGated,
    [switch]$CaptureSatellitePasses,
    [switch]$TraceGameplayDraws,
    [switch]$CaptureDrawGated,
    [switch]$DumpGameplayGeometry,
    [switch]$CaptureGameplayAnyTarget,
    [switch]$CaptureGameplayAllTargets,
    [switch]$CaptureGameplayDepthClear,
    [switch]$CaptureTextureAlpha,
    [ValidateRange(0, 5000)]
    [int]$CaptureGameplayDrawStride,
    [ValidateRange(0, 5000)]
    [int]$CaptureGameplayDrawMin,
    [ValidateRange(0, 5000)]
    [int]$CaptureGameplayDrawLimit,
    [ValidateRange(1, 120)]
    [int]$CaptureGameplayDrawFrame = 5,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$BootMetadataRun,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$CheckpointRun,
    [switch]$TestMouse,
    [switch]$TestPrompts,
    [switch]$TraceSaveIo,
    [switch]$TraceNotificationEntries,
    [switch]$TraceNotificationList,
    [switch]$TraceZephyrAnim,
    [switch]$TraceAiStack,
    [switch]$TraceCollisionAgent,
    [switch]$TraceVehicleDoor,
    [switch]$TraceTargetManager,
    [switch]$TraceTeardownStack,
    [switch]$TraceEventAbi,
    [switch]$TraceVoiceCallbacks,
    [switch]$TraceTransitionPool,
    [switch]$TraceTempArray,
    [switch]$TraceParticleList,
    [switch]$TraceTrafficSpawn,
    [ValidatePattern('^[0-9A-Fa-f]{8}$')]
    [string]$TraceTrafficZone,
    [switch]$TraceForcedWalk,
    [switch]$TraceDataPod,
    [switch]$TraceSecondaryAmmo,
    [switch]$TraceXactAllocs,
    [switch]$TraceAudioMixAudit,
    [switch]$TraceHashTable,
    [switch]$TracePdaStore,
    [switch]$CapturePcm,
    [switch]$UnmutedAudio,
    [switch]$CaptureDisplay,
    [ValidateRange(250, 60000)]
    [int]$CaptureDisplayIntervalMs = 2000,
    [ValidateRange(1, 256)]
    [int]$CaptureDisplayCount = 12,
    [switch]$TracePresentFlash,
    [switch]$TraceScanoutSurface,
    [ValidatePattern('^(?:0x)?[0-9A-Fa-f]{5,8}$')]
    [string]$ScanoutGuestDword,
    [switch]$TraceFlips,
    [ValidateRange(1, 64)]
    [int]$PresentFlashCaptureCount = 32,
    [switch]$TraceApuHeadroom,
    [switch]$TraceApuMixbins,
    [switch]$ReferenceVpMix,
    [switch]$DumpApuDspPram,
    [switch]$TraceApuDspN5,
    [ValidateRange(1000, 100000000)]
    [int]$ApuDspCycleLimit,
    [switch]$TraceD3DStateCompiler,
    [switch]$ColdShaderCache,
    [switch]$TraceRedScene,
    [switch]$TraceHavokHeightfield,
    [switch]$TraceHavokAllocatorUnderflow,
    [switch]$TraceRedSceneWriteWatch,
    [ValidateRange(0, 4095)]
    [int]$RedSceneWriteIndex = 1891,
    [switch]$TraceSehEbp,
    [switch]$TraceFrontendRecentExit,
    [switch]$TraceMenuPaint,
    [switch]$TracePresentTiming,
    [switch]$PerformanceTrace,
    [switch]$TraceVertexWarmup,
    [switch]$TraceVehicleFrames,
    [switch]$TraceCameraTransition,
    [switch]$TraceScreenFlash,
    [switch]$TraceSatelliteColor,
    [switch]$TestSurvivorFlash,
    [ValidatePattern('^(?:0x)?[0-9A-Fa-f]{5,8}$')]
    [string]$TracePointerWatchVa,
    [switch]$TraceD3DDrawPerf,
    [switch]$TraceDepthAliasStencil,
    [switch]$TraceSkyState,
    [switch]$DisableStaticLightRefresh,
    [switch]$TraceGamma,
    [switch]$CaptureShadowQuad,
    [ValidateRange(1, 64)]
    [int]$CaptureShadowOrdinal = 1,
    [switch]$CaptureCanvasMask,
    [ValidatePattern('^[0-9A-Fa-f]{8}$')]
    [string]$CaptureShaderHash,
    [ValidateRange(1, 120)]
    [int]$CaptureShaderOrdinal = 8,
    [ValidateRange(0, 65536)]
    [int]$CaptureShaderVertices = 0,
    [switch]$TracePolygonOffset,
    [switch]$StrictScreenRounding,
    [switch]$TestDevRegionImport,
    [switch]$TestGuestDepth,
    [switch]$TestGuestDepthGated,
    [switch]$TestPreciseDepth,
    [switch]$TestPreciseDepthGated,
    [switch]$TraceSurfaceAliases,
    [switch]$CaptureFlareComposite,
    [switch]$CaptureFlareProbe,
    [switch]$TraceSuspiciousFlare,
    [switch]$DisableFlipPacing,
    [switch]$TestInternalResolutionTransitions,
    [switch]$TestPresentationResizeTransitions,
    [ValidateRange(0, 600000)]
    [int]$InternalResolutionTransitionDelayMs = 0,
    [ValidateRange(1, 1000000)]
    [int]$TraceFrontendPostLuaCount,
    [switch]$ExactLoadSave,
    [switch]$TrackRecompEntries,
    [switch]$TraceKernelCalls,
    [switch]$TraceGameProgress,
    [ValidateRange(50, 60000)]
    [int]$TraceTimeoutMs,
    [ValidateRange(1000, 60000)]
    [int]$TraceGameStallMs,
    [ValidateRange(1, 512)]
    [int]$TraceFileOpenDumpAt,
    [ValidateRange(1, 1000000)]
    [int]$TraceKernelDumpAt,
    [ValidateRange(1, 256)]
    [int]$TrafficRandomPathTargetCap,
    [ValidatePattern('^[0-9]{1,4}$')]
    [string]$CollisionWriteSlot,
    [switch]$WatchHumanNotificationOwners,
    [ValidatePattern('^(?:auto|(?:0x)?[0-9A-Fa-f]{5,8}:(?:0x)?[0-9A-Fa-f]{5,8})$')]
    [string]$WatchNotificationOwner,
    [ValidatePattern('^(?:0x)?[0-9A-Fa-f]{5,8}$')]
    [string]$WatchGuestDword,
    [switch]$TraceWatchRecent,
    [switch]$Visible
)

# Manual controller-file tests only: no focus changes, automatic New Game,
# keyboard injection, or user save directories. A checkpoint is copied into
# a new private root; the source run and user preview saves are never mounted.
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$executable = Join-Path $workspace "build\mercenaries\bin\Release\$CandidateName.exe"
$gameDirectory = Join-Path $workspace 'game_files\mercenaries-retail'
if ($TestGameDirectory) {
    $testDisc = Get-Item -LiteralPath $TestGameDirectory
    $testRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'artifacts\test-runs')) + [IO.Path]::DirectorySeparatorChar
    if (-not $testDisc.PSIsContainer -or -not $testDisc.FullName.StartsWith($testRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test disc must be inside private test-runs' }
    $testItems = @($testDisc) + @(Get-ChildItem -LiteralPath $testDisc.FullName -Recurse -Force)
    if ($testItems | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw 'Test disc cannot redirect through reparse points' }
    $gameDirectory = $testDisc.FullName
}

$runDirectory = Join-Path $workspace "artifacts\test-runs\$RunName"
if ($CheckpointRun -and $BootMetadataRun) { throw 'Choose checkpoint or boot metadata, not both' }
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Candidate missing' }
if (-not (Test-Path -LiteralPath (Join-Path $gameDirectory 'default.xbe'))) { throw 'Retail XBE missing' }
if (Test-Path -LiteralPath $runDirectory) { throw 'Use a new diagnostic run directory' }
New-Item -ItemType Directory -Path $runDirectory | Out-Null
if ($OptionsFile) {
    # Copy settings beside a private executable; never rewrite a live installation.
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
$logDirectory = Join-Path ([IO.Path]::GetTempPath()) "mercenaries-recomp-runs\$RunName"
if (Test-Path -LiteralPath $logDirectory) { throw 'Diagnostic log directory already exists' }
New-Item -ItemType Directory -Path $logDirectory | Out-Null
if ($BootMetadataRun) {
    # Existing private partition/cache initialization avoids the currently
    # unimplemented Xbox format-volume reboot path. No UserData/TitleData,
    # UDATA/TDATA save containers, profiles, or progress are copied.
    $sourceRoot = Join-Path $workspace "artifacts\test-runs\$BootMetadataRun\saves"
    $saveRoot = Join-Path $runDirectory 'saves'
    New-Item -ItemType Directory -Path $saveRoot | Out-Null
    foreach ($directory in @('System', 'Cache')) {
        $source = Join-Path $sourceRoot $directory
        if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Missing private boot metadata: $source" }
        Copy-Item -LiteralPath $source -Destination $saveRoot -Recurse -Force
    }
}
if ($CheckpointRun) {
    $sourceRun = Join-Path $workspace "artifacts\test-runs\$CheckpointRun"
    $sourceRoot = Join-Path $sourceRun 'saves'
    if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) { throw 'Private checkpoint root missing' }
    # Reject links: a diagnostic name must not redirect to preview/user data.
    $sourceItems = @((Get-Item -LiteralPath $sourceRun), (Get-Item -LiteralPath $sourceRoot)) + @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -Force)
    if ($sourceItems | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw 'Checkpoint must contain only ordinary private directories/files'
    }
    $payload = Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'UserData') -Recurse -File |
        Where-Object { $_.Name -eq 'Mercenaries Saves' -and $_.Length -gt 512 }
    if (-not $payload) { throw 'No game-written save payload in private checkpoint' }
    Copy-Item -LiteralPath $sourceRoot -Destination (Join-Path $runDirectory 'saves') -Recurse
}
$settings = @{
    MERCENARIES_TEST_SAVE_DIR = (Join-Path $runDirectory 'saves')
    MERCENARIES_DIAGNOSTIC_BUFFERED_LOGS = '1'
    MERCENARIES_TEST_GAMEPAD_FILE = (Join-Path $runDirectory 'gamepad-command.txt')
    MERCENARIES_TEST_SPAWN_FILE = (Join-Path $runDirectory 'spawn-command.txt')
    MERCENARIES_CAPTURE_TEST_GAMEPAD_PREFIX = (Join-Path $runDirectory 'gamepad')
    MERCENARIES_TRACE_TEST_INPUT = '1'
    # This harness exists for acknowledged menu interactions; record the
    # retail menu and Load/Save stack transitions alongside each command.
    MERCENARIES_TRACE_INPUT = '1'
    MERCENARIES_DUMP_CRASH_GUEST_PATH = (Join-Path $runDirectory 'crash-guest.bin')
}
if ($TestMouse) {
    if ($Visible) { throw 'Synthetic mouse testing requires isolated hidden input' }
    $settings.MERCENARIES_TEST_MOUSE_FILE = (Join-Path $runDirectory 'mouse-command.txt')
}
if (-not $Visible) {
    $settings.MERCENARIES_HIDE_WINDOW = '1'
    $settings.MERCENARIES_TEST_ISOLATE_INPUT = '1'
    if ($TestPrompts) { $settings.MERCENARIES_TEST_PROMPT_FILE = (Join-Path $runDirectory 'prompt-device.txt') }
    # Process-loopback comparisons require audible output; isolation stays on.
    if (-not $UnmutedAudio) { $settings.MERCENARIES_TEST_MUTE_HOST_AUDIO = '1' }
}
if ($ColdShaderCache) {
    $settings.LOCALAPPDATA = Join-Path $runDirectory 'local-appdata'
    New-Item -ItemType Directory -Path $settings.LOCALAPPDATA | Out-Null
}
if ($ResolutionScaleOverride -ge 0) {
    $settings.MERCENARIES_TEST_RESOLUTION_SCALE = [string]$ResolutionScaleOverride
}
if ($PSBoundParameters.ContainsKey('TraceSurfaceTexturesAfterMs')) {
    $settings.MERCENARIES_TRACE_SURFACE_TEXTURES = '1'
    $settings.MERCENARIES_TRACE_SURFACE_TEXTURE_REJECTS = '1'
    $settings.MERCENARIES_TRACE_SURFACE_METHODS = '1'
    $settings.MERCENARIES_TRACE_SURFACE_DELAY_MS =
        [string]$TraceSurfaceTexturesAfterMs
}
if ($DumpGameplayGeometry) {
    $settings.MERCENARIES_CAPTURE_GAMEPLAY_GEOMETRY_ONLY = '1'
}
if ($CaptureDrawGated) {
    $settings.MERCENARIES_CAPTURE_DRAW_GATE_REQUIRED = '1'
    $settings.MERCENARIES_CAPTURE_DRAW_GATE_FILE = Join-Path $runDirectory 'draw-capture.ready'
}
if ($TraceGameplayDraws) {
    $settings.MERCENARIES_TRACE_GAMEPLAY_DRAWS = '1'
    $settings.MERCENARIES_CAPTURE_GAMEPLAY_DRAW_FRAME =
        [string]$CaptureGameplayDrawFrame
}
if ($CaptureGameplayAnyTarget) {
    $settings.MERCENARIES_CAPTURE_GAMEPLAY_ANY_TARGET = '1'
}
if ($CaptureGameplayDepthClear) {
    $settings.MERCENARIES_CAPTURE_GAMEPLAY_DEPTH_CLEAR = '1'
}
if ($CaptureGameplayAllTargets) {
    $settings.MERCENARIES_CAPTURE_GAMEPLAY_ALL_TARGETS = '1'
}
if ($CaptureTextureAlpha) {
    $settings.MERCENARIES_CAPTURE_TEXTURE_ALPHA = '1'
}
if ($CaptureCanvasMask) { $settings.MERCENARIES_CAPTURE_CANVAS_MASK = '1' }
if ($CaptureShaderVertices) { $settings.MERCENARIES_CAPTURE_SHADER_VERTICES = [string]$CaptureShaderVertices }
if ($CaptureShaderHash) { $settings.MERCENARIES_CAPTURE_SHADER_HASH = $CaptureShaderHash; $settings.MERCENARIES_CAPTURE_SHADER_ORDINAL = [string]$CaptureShaderOrdinal }
if ($CaptureGameplayDrawStride -or $CaptureShadowQuad -or $CaptureCanvasMask -or $CaptureShaderHash) {
    $settings.MERCENARIES_CAPTURE_GAMEPLAY_DRAW_PREFIX =
        (Join-Path $runDirectory 'draw')
    $settings.MERCENARIES_CAPTURE_GAMEPLAY_DRAW_STRIDE =
        [string]$CaptureGameplayDrawStride
    $settings.MERCENARIES_CAPTURE_GAMEPLAY_DRAW_FRAME =
        [string]$CaptureGameplayDrawFrame
    if ($PSBoundParameters.ContainsKey('CaptureGameplayDrawMin')) {
        $settings.MERCENARIES_CAPTURE_GAMEPLAY_DRAW_MIN =
            [string]$CaptureGameplayDrawMin
    }
    if ($PSBoundParameters.ContainsKey('CaptureGameplayDrawLimit')) {
        $settings.MERCENARIES_CAPTURE_GAMEPLAY_DRAW_LIMIT =
            [string]$CaptureGameplayDrawLimit
    }
}
if ($TraceSaveIo) {
    $settings.MERCENARIES_TRACE_SAVE_IO = '1'
}
$previous = @{}
if ($TraceNotificationEntries) {
    $settings.MERCENARIES_TRACE_NOTIFICATION_ENTRIES = '1'
}
if ($TraceNotificationList) {
    $settings.MERCENARIES_TRACE_NOTIFICATION_LIST = '1'
}
if ($TraceZephyrAnim) {
    $settings.MERCENARIES_TRACE_ZEPHYR_ANIM = '1'
}
if ($TraceAiStack) {
    # Read-only mismatch observer around the retail AI actor-update boundary.
    # Pairing this with Zephyr handle tracing distinguishes a missing AI tick
    # from a valid tick that selects or samples an invalid animation.
    $settings.MERCENARIES_TRACE_AI_UPDATE_STACK = '1'
}
if ($TraceCollisionAgent) {
    $settings.MERCENARIES_TRACE_COLLISION_AGENT = '1'
}
if ($TraceVehicleDoor) {
    $settings.MERCENARIES_TRACE_VEHICLE_DOOR = '1'
}
if ($TraceTargetManager) {
    $settings.MERCENARIES_TRACE_TARGET_MANAGER = '1'
}
if ($TraceTeardownStack) {
    $settings.MERCENARIES_TRACE_TEARDOWN_STACK = '1'
}
if ($TraceTransitionPool) {
    $settings.MERCENARIES_TRACE_TRANSITION_POOL_COUNT = '1'
}
if ($TraceTempArray) {
    $settings.MERCENARIES_TRACE_TEMP_ARRAY = '1'
}
if ($TraceParticleList) {
    $settings.MERCENARIES_TRACE_PARTICLE_LIST = '1'
}
if ($TraceTrafficSpawn -or $TraceTrafficZone) {
    $settings.MERCENARIES_TRACE_TRAFFIC_SPAWN = '1'
}
if ($TraceTrafficZone) {
    $settings.MERCENARIES_TRACE_TRAFFIC_ZONE = $TraceTrafficZone
}
if ($TraceForcedWalk) {
    $settings.MERCENARIES_TRACE_FORCED_WALK = '1'
}
if ($TraceDataPod) {
    $settings.MERCENARIES_TRACE_DATAPOD_FLASH = '1'
}
if ($TraceSecondaryAmmo) {
    $settings.MERCENARIES_TRACE_SECONDARY_AMMO = '1'
}
if ($TraceXactAllocs) {
    $settings.MERCENARIES_TRACE_XACT_ALLOCS = '1'
}
if ($TraceHashTable) {
    $settings.MERCENARIES_TRACE_HASH_TABLE = '1'
}
if ($TracePdaStore) {
    $settings.MERCENARIES_TRACE_PDA_STORE = '1'
}
if ($TraceAudioMixAudit) {
    $settings.MERCENARIES_TRACE_APU_PITCH = '1'
    $settings.MERCENARIES_TRACE_APU_VOICE_ON = '1'
    $settings.MERCENARIES_TRACE_APU_VOICE_ENERGY = '1'
    $settings.MERCENARIES_TRACE_XACT_CUES = '1'
    $settings.MERCENARIES_TRACE_XACT_BANKS = '1'
}
if ($CapturePcm) {
    $settings.MERCENARIES_DUMP_XAUDIO_PCM_PATH =
        (Join-Path $runDirectory 'host-output-s16le-48k-stereo.pcm')
}
if ($CaptureDisplay) {
    $settings.MERCENARIES_CAPTURE_DISPLAY_PREFIX =
        (Join-Path $runDirectory 'display')
    $settings.MERCENARIES_CAPTURE_DISPLAY_INTERVAL_MS =
        [string]$CaptureDisplayIntervalMs
    $settings.MERCENARIES_CAPTURE_DISPLAY_COUNT =
        [string]$CaptureDisplayCount
}
if ($TracePresentFlash) {
    $settings.MERCENARIES_TRACE_PRESENT_FLASH = '1'
    $settings.MERCENARIES_TRACE_PRESENT_FLASH_GATE_FILE =
        (Join-Path $runDirectory 'present-flash.gate')
    $settings.MERCENARIES_CAPTURE_PRESENT_FLASH_PREFIX =
        (Join-Path $runDirectory 'present-flash')
    $settings.MERCENARIES_CAPTURE_PRESENT_FLASH_COUNT =
        [string]$PresentFlashCaptureCount
}
if ($TraceScanoutSurface) {
    $settings.MERCENARIES_TRACE_SCANOUT_SURFACE = '1'
}
if ($TraceFlips) {
    $settings.MERCENARIES_TRACE_FLIPS = '1'
    $settings.MERCENARIES_TRACE_PRESENT_SURFACE = '1'
}if ($TraceApuHeadroom) {
    $settings.MERCENARIES_TRACE_APU_HEADROOM = '1'
}
if ($DumpApuDspPram) {
    $settings.MERCENARIES_DUMP_APU_DSP_PRAM = '1'
    $settings.MERCENARIES_ENABLE_APU_DSP = '1'
}
if ($TraceApuDspN5) {
    $settings.MERCENARIES_TRACE_APU_DSP_N5 = '1'
    $settings.MERCENARIES_ENABLE_APU_DSP = '1'
}
if ($ApuDspCycleLimit) {
    $settings.MERCENARIES_TEST_APU_DSP_CYCLE_LIMIT = [string]$ApuDspCycleLimit
    $settings.MERCENARIES_ENABLE_APU_DSP = '1'
}
if ($TraceD3DStateCompiler) {
    $settings.MERCENARIES_TRACE_D3D_STATE_COMPILER = '1'
}
if ($ScanoutGuestDword) {
    $settings.MERCENARIES_TRACE_SCANOUT_GUEST_DWORD = $ScanoutGuestDword
}
if ($TraceRedScene) {
    $settings.MERCENARIES_TRACE_REDSCENE = '1'
}
if ($TraceHavokHeightfield) {
    $settings.MERCENARIES_TRACE_HAVOK_HEIGHTFIELD = '1'
}
if ($TraceHavokAllocatorUnderflow) {
    $settings.MERCENARIES_TRACE_HAVOK_ALLOCATOR_UNDERFLOW = '1'
}
if ($TraceRedSceneWriteWatch) {
    $settings.MERCENARIES_TRACE_REDSCENE = '1'
    $settings.MERCENARIES_TRACE_REDSCENE_WRITE_WATCH = '1'
    $settings.MERCENARIES_TRACE_REDSCENE_WRITE_INDEX = [string]$RedSceneWriteIndex
}
if ($TraceSehEbp) {
    $settings.MERCENARIES_TRACE_SEH_EBP = '1'
}
if ($TraceFrontendRecentExit) {
    $settings.MERCENARIES_TRACE_FRONTEND_RECENT_EXIT = '1'
}
if ($TraceMenuPaint) {
    $settings.MERCENARIES_TRACE_MENU_PAINT = '1'
}
if ($TracePresentTiming) {
    $settings.MERCENARIES_TRACE_PRESENT_TIMING = '1'
}
if ($TracePointerWatchVa) { $settings.MERCENARIES_TRACE_POINTER_WATCH_VA = $TracePointerWatchVa }
if ($TraceVehicleFrames) {
    $settings.MERCENARIES_TRACE_VEHICLE_FRAMES = '1'
}
if ($TraceVertexWarmup) { $settings.MERCENARIES_TRACE_VSH_WARMUP = '1' }
if ($PerformanceTrace) {
    $settings.Remove('MERCENARIES_TRACE_INPUT')
    $settings.Remove('MERCENARIES_TRACE_TEST_INPUT')
    $settings.MERCENARIES_TRACE_PRESENT_TIMING = '1'
    $settings.MERCENARIES_TRACE_D3D_DRAW_PERF = '1'
    $settings.MERCENARIES_TRACE_SHADER_TIMING = '1'
    $settings.MERCENARIES_TRACE_TEXTURE_PERF = '1'
    $settings.MERCENARIES_TRACE_FILE_READS = '1'
}
if ($TraceD3DDrawPerf) {
    $settings.MERCENARIES_TRACE_D3D_DRAW_PERF = '1'
}
if ($TraceDepthAliasStencil) {
    $settings.MERCENARIES_TRACE_DEPTH_ALIAS_STENCIL = '1'
}
if ($StrictScreenRounding) { $settings.MERCENARIES_STRICT_SCREEN_ROUNDING = '1' }
if ($TestDevRegionImport) { $settings.MERCENARIES_DEV_TEST_REGION_IMPORT = '1' }
if ($TestGuestDepth -or $TestGuestDepthGated) { $settings.MERCENARIES_TEST_GUEST_DEPTH = '1' }
if ($TestGuestDepthGated) {
    $settings.MERCENARIES_TEST_GUEST_DEPTH_GATE_FILE = Join-Path $runDirectory 'guest-depth.gate'
    Set-Content -LiteralPath $settings.MERCENARIES_TEST_GUEST_DEPTH_GATE_FILE -Value '1'
}
if ($TestPreciseDepth -or $TestPreciseDepthGated) { $settings.MERCENARIES_TEST_PRECISE_DEPTH = '1' }
if ($TestPreciseDepthGated) {
    $settings.MERCENARIES_TEST_PRECISE_DEPTH_GATE_FILE = Join-Path $runDirectory 'precise-depth.gate'
    Set-Content -LiteralPath $settings.MERCENARIES_TEST_PRECISE_DEPTH_GATE_FILE -Value '1'
}
if ($TracePolygonOffset) {
    $settings.MERCENARIES_TRACE_POLYGON_OFFSET = '1'
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
if ($CaptureFlareComposite) {
    $settings.MERCENARIES_CAPTURE_FLARE_COMPOSITE_PREFIX =
        (Join-Path $runDirectory 'flare-composite')
    $settings.MERCENARIES_CAPTURE_FLARE_COMPOSITE_GATE_FILE =
        (Join-Path $runDirectory 'flare-capture.gate')
}
if ($CaptureFlareProbe) {
    $settings.MERCENARIES_CAPTURE_FLARE_PROBE_PREFIX =
        (Join-Path $runDirectory 'flare-probe')
    $settings.MERCENARIES_CAPTURE_FLARE_PROBE_GATE_FILE =
        (Join-Path $runDirectory 'flare-capture.gate')
}
if ($TraceSuspiciousFlare) {
    $settings.MERCENARIES_TRACE_SUSPICIOUS_FLARE_BOUNDS = '8'
    $settings.MERCENARIES_TRACE_SUSPICIOUS_FLARE_GATE_FILE =
        (Join-Path $runDirectory 'flare-capture.gate')
    $settings.MERCENARIES_CAPTURE_SUSPICIOUS_FLARE_PREFIX =
        (Join-Path $runDirectory 'flare-suspicious')
}
if ($DisableFlipPacing) {
    $settings.MERCENARIES_DISABLE_FLIP_PACING = '1'
}
if ($TestInternalResolutionTransitions) {
    $settings.MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS = '1'
    $settings.MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS_DELAY_MS =
        [string]$InternalResolutionTransitionDelayMs
    $settings.MERCENARIES_TEST_INTERNAL_RESOLUTION_CAPTURE_PREFIX =
        (Join-Path $runDirectory 'resolution-transition')
}
if ($TestPresentationResizeTransitions) {
    if (-not $TestInternalResolutionTransitions) {
        throw 'Presentation resize transitions require internal resolution transitions'
    }
    $settings.MERCENARIES_TEST_PRESENTATION_RESIZE_TRANSITIONS = '1'
}
if ($TraceFrontendPostLuaCount) {
    $settings.MERCENARIES_TRACE_FRONTEND_POST_LUA_COUNT = [string]$TraceFrontendPostLuaCount
}
if ($ExactLoadSave) {
    $settings.MERCENARIES_TEST_EXACT_LOAD_GAME_SEQUENCE = '1'
    $settings.MERCENARIES_TEST_EXACT_LOAD_SINGLE_POLL = '1'
    $settings.MERCENARIES_CAPTURE_GAME_PATH = (Join-Path $runDirectory 'loaded-save.bmp')
    $settings.MERCENARIES_CAPTURE_GAME_DELAY_MS = '5000'
}

if ($TrackRecompEntries) {
    $settings.MERCENARIES_TRACK_RECOMP_ENTRIES = '1'
}
if ($TraceKernelCalls) {
    $settings.MERCENARIES_TRACE_KERNEL_CALLS = '1'
}
if ($TraceGameProgress) {
    $settings.MERCENARIES_TRACE_GAME_PROGRESS = '1'
}
if ($TraceFullscreenImmediateGated) {
    $settings.MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE = '1'
    $settings.MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE_GATE_FILE =
        (Join-Path $runDirectory 'fullscreen-trace.ready')
}
if ($CaptureSatellitePasses) {
    $settings.MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX =
        (Join-Path $runDirectory 'satellite-pass')
    $settings.MERCENARIES_CAPTURE_SATELLITE_PASS_GATE_FILE =
        (Join-Path $runDirectory 'satellite-pass.ready')
}
if ($TraceTimeoutMs) {
    $settings.MERCENARIES_TRACE_TIMEOUT_MS = [string]$TraceTimeoutMs
}
if ($TraceGameStallMs) {
    $settings.MERCENARIES_TRACE_GAME_STALL_MS = [string]$TraceGameStallMs
}
if ($TraceFileOpenDumpAt) {
    $settings.MERCENARIES_TRACE_FILE_OPEN_DUMP_AT = [string]$TraceFileOpenDumpAt
}
if ($TraceKernelDumpAt) {
    $settings.MERCENARIES_TRACE_KERNEL_DUMP_AT = [string]$TraceKernelDumpAt
}
if ($TrafficRandomPathTargetCap) {
    $settings.MERCENARIES_TEST_TRAFFIC_RANDOM_PATH_TARGET_CAP = [string]$TrafficRandomPathTargetCap
}
if ($CollisionWriteSlot) {
    if ([int]$CollisionWriteSlot -gt 4095) { throw 'Collision write slot out of range' }
    $settings.MERCENARIES_COLLISION_WRITE_SLOT = $CollisionWriteSlot
}
if ($WatchHumanNotificationOwners) {
    $settings.MERCENARIES_WATCH_NOTIFICATION_OWNER = 'dispatch'
} elseif ($WatchNotificationOwner) {
    $settings.MERCENARIES_WATCH_NOTIFICATION_OWNER = $WatchNotificationOwner
}
if ($WatchGuestDword) {
    $settings.MERCENARIES_WATCH_GUEST_DWORD = $WatchGuestDword
}
if ($TraceWatchRecent) {
    $settings.MERCENARIES_TRACE_WATCH_RECENT = '1'
}
if ($CaptureShadowQuad) { $settings.MERCENARIES_CAPTURE_SHADOW_QUAD = '1'; $settings.MERCENARIES_CAPTURE_SHADOW_ORDINAL = [string]$CaptureShadowOrdinal }
if ($TraceCameraTransition) { $settings.MERCENARIES_TRACE_CAMERA_TRANSITION = '1' }
if ($TraceScreenFlash) { $settings.MERCENARIES_TRACE_SCREEN_FLASH = '1' }
if ($TraceSatelliteColor) { $settings.MERCENARIES_TRACE_SATELLITE_COLOR = '1' }
if ($TraceApuMixbins) { $settings.MERCENARIES_TRACE_APU_MIXBINS = '1' }
if ($ReferenceVpMix) { $settings.MERCENARIES_DISABLE_APU_SPEAKER_DOWNMIX = '1' }
if ($TestSurvivorFlash) {
    # One-shot original HUD flash only; does not damage the private player.
    $flashTrigger = Join-Path $runDirectory 'survivor-flash.ready'
    $settings.MERCENARIES_TEST_SURVIVOR_FLASH_FILE = $flashTrigger
    $settings.MERCENARIES_TEST_SURVIVOR_FLASH_CAPTURE_PREFIX = Join-Path $runDirectory 'flash-sequence'
    $settings.MERCENARIES_TRACE_SCREEN_FLASH = '1'
    $settings.MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE = '1'
    $settings.MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE_GATE_FILE = $flashTrigger
    $settings.MERCENARIES_CAPTURE_BLEND_EQUATION_PATH = Join-Path $runDirectory 'survivor-flash.bmp'
    $settings.MERCENARIES_CAPTURE_BLEND_EQUATION_GATE_FILE = $flashTrigger
}
if ($TraceGamma) { $settings.MERCENARIES_TRACE_GAMMA = '1' }
if ($DisableStaticLightRefresh) { $settings.MERCENARIES_DISABLE_STATIC_LIGHT_REFRESH = '1' }
if ($TraceEventAbi) { $settings.MERCENARIES_TRACE_EVENT_ABI = '1' }
if ($TraceVoiceCallbacks) {
    $settings.MERCENARIES_TRACE_VOICE_CALLBACK = '1'
    $settings.MERCENARIES_TRACE_XACT_PLAY = '1'
    $settings.MERCENARIES_TRACE_XACT_PROPERTIES = '1'
}
$settings.MERCENARIES_PREVIEW_LOG_DIR = Join-Path $runDirectory 'preview-logs'
foreach ($entry in Get-ChildItem Env:MERCENARIES_*) { $previous[$entry.Name] = $entry.Value }
try {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    foreach ($entry in $settings.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process') }
    Set-Content -LiteralPath $settings.MERCENARIES_TEST_GAMEPAD_FILE -Value '0 1 1 0 0 0 0 0 0 0'
    $windowStyle = if ($Visible) { 'Normal' } else { 'Hidden' }
    $process = Start-Process -FilePath $executable -ArgumentList ('"' + $gameDirectory + '"') `
        -WorkingDirectory $workspace -WindowStyle $windowStyle -PassThru `
        -RedirectStandardOutput (Join-Path $logDirectory 'stdout.log') `
        -RedirectStandardError (Join-Path $logDirectory 'stderr.log')
    Set-Content -LiteralPath (Join-Path $runDirectory 'pid.txt') -Value $process.Id
    Set-Content -LiteralPath (Join-Path $runDirectory 'log-spool-path.txt') -Value $logDirectory
    [pscustomobject]@{ProcessId=$process.Id; RunDirectory=$runDirectory; LogDirectory=$logDirectory}
} finally {
    foreach ($name in $settings.Keys) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    foreach ($entry in $previous.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process') }
}


