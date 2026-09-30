/*
 * kernel_xbox.c - Xbox Identity & Hardware Stubs
 *
 * Provides Xbox-specific exported data (hardware info, kernel version,
 * keys, image filename) and section loading stubs.
 *
 * Most Xbox-specific hardware data is stubbed with plausible retail
 * values. Crypto keys are zeroed since we don't need Xbox Live or
 * EEPROM-based encryption on PC.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* localtime_r and timegm */
#endif
#include "kernel.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#endif

/* Keep the virtual console's user video settings independent from the host
 * window size. Retail games use these EEPROM flags to choose their own 4:3
 * or 16:9 layouts; reporting widescreen unconditionally visibly distorts
 * titles that render a native 640x480 presentation. */
static int xbox_video_option_enabled(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
}

static int xbox_widescreen_override = -1;

void xbox_set_widescreen_enabled(BOOL enabled)
{
    xbox_widescreen_override = enabled != FALSE;
}

ULONG xbox_get_user_video_flags(void)
{
    ULONG flags = 0;
    if (xbox_widescreen_override > 0 ||
        (xbox_widescreen_override < 0 &&
         xbox_video_option_enabled("XBOXRECOMP_VIDEO_WIDESCREEN")))
        flags |= XC_VIDEO_FLAGS_WIDESCREEN;
    if (xbox_video_option_enabled("XBOXRECOMP_VIDEO_HDTV"))
        flags |= XC_VIDEO_FLAGS_HDTV;
    return flags;
}

ULONG xbox_get_av_pack(void)
{
    return xbox_video_option_enabled("XBOXRECOMP_VIDEO_HDTV")
        ? AV_PACK_HDTV : AV_PACK_STANDARD;
}

/* ============================================================================
 * Exported Data Objects
 *
 * These are global variables exported by the Xbox kernel at known ordinals.
 * Game code accesses them directly via the thunk table.
 * ============================================================================ */

/* Hardware info - report a standard 1.0 retail Xbox */
XBOX_HARDWARE_INFO xbox_HardwareInfo = {
    .Flags       = 0x00000020,  /* Retail Xbox */
    .GpuRevision = 0xD2,       /* NV2A D2 revision */
    .McpRevision = 0xD4,       /* MCPX D4 revision */
    .Reserved    = {0, 0}
};

/* Kernel version - match XDK 5849 (the version Burnout 3 was built against) */
XBOX_KRNL_VERSION xbox_KrnlVersion = {
    .Major = 1,
    .Minor = 0,
    .Build = 5849,
    .Qfe   = 1
};

/* Crypto keys - zeroed, not needed for PC operation.
 * These are unique per-console on real hardware. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* Public key data for Xbox Live signature verification - not needed */
UCHAR xbox_XePublicKeyData[284] = {0};

/* Image filename - the XBE path as seen by the kernel */
static char g_image_filename[] = "\\Device\\CdRom0\\default.xbe";
XBOX_ANSI_STRING xbox_XeImageFileName = {
    .Length        = sizeof(g_image_filename) - 1,
    .MaximumLength = sizeof(g_image_filename),
    .Buffer        = g_image_filename
};

/* Launch data page - used for title-to-title launches (e.g., Xbox Dashboard → game).
 * Allocated dynamically and zeroed for normal game boot. */
static XBOX_LAUNCH_DATA_PAGE g_launch_data_page = {0};
XBOX_LAUNCH_DATA_PAGE* xbox_LaunchDataPage = &g_launch_data_page;

/* ============================================================================
 * Section Loading
 *
 * XeLoadSection/XeUnloadSection manage on-demand loading of XBE sections.
 * On Xbox, some sections are demand-paged from disc. In our recompilation,
 * the entire executable is loaded into memory, so these are reference-counting
 * no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_XeLoadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    InterlockedIncrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeLoadSection: '%s' at %p (size=%u, refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        Section->VirtualAddress, Section->VirtualSize,
        Section->SectionReferenceCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_XeUnloadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    LONG new_count = InterlockedDecrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeUnloadSection: '%s' (refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        new_count);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * EEPROM / Non-Volatile Settings
 *
 * ExQueryNonVolatileSetting / ExSaveNonVolatileSetting read and write EEPROM
 * settings. On real hardware these are stored in the 256-byte EEPROM on the
 * SMBus. For recompilation, we return sensible defaults:
 *   - Region: North America
 *   - Video: NTSC, widescreen+HDTV enabled
 *   - Language: English
 *   - Audio: Stereo, Dolby Digital
 *   - DVD Region: Region 1 (North America)
 * ============================================================================ */

/* XAPI subtracts this bias from UTC. Use the host's current effective offset
 * (including DST), rather than the console's old daylight-saving rules. */
static LONG xbox_current_timezone_bias(void)
{
#ifdef _WIN32
    TIME_ZONE_INFORMATION zone;
    DWORD state = GetTimeZoneInformation(&zone);
    if (state == TIME_ZONE_ID_INVALID)
        return 0;
    return zone.Bias + (state == TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias :
                        state == TIME_ZONE_ID_STANDARD ? zone.StandardBias : 0);
#else
    time_t now = time(NULL);
    struct tm local;
    if (!localtime_r(&now, &local))
        return 0;
    return (LONG)(difftime(now, timegm(&local)) / 60.0);
#endif
}

/* XAPI's GetLocalTime reads the bulk user section, not XC_TIMEZONE_BIAS.
 * Offsets here are relative to the user checksum at EEPROM offset 0x60. */
static void xbox_get_user_settings(ULONG settings[24])
{
    memset(settings, 0, 24 * sizeof(*settings));
    settings[1] = (ULONG)xbox_current_timezone_bias();
    /* Transition dates and additional seasonal biases stay zero: the base
     * bias already includes the host's current daylight-saving adjustment. */
    settings[12] = 1; /* language: English */
    settings[13] = xbox_get_user_video_flags();
    settings[14] = 0x00010001; /* audio */
    settings[23] = 1; /* DVD region */
}

NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{
    if (!Value)
        return STATUS_INVALID_PARAMETER;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "ExQueryNonVolatileSetting: index=0x%02X len=%u", ValueIndex, ValueLength);

    switch (ValueIndex) {
    case XC_LANGUAGE:
        /* English = 1 */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_VIDEO:
        /* Retail NTSC 4:3 by default; optional modes model user EEPROM state. */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = xbox_get_user_video_flags();
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;
    case XC_FACTORY_AV_REGION:
        /* Retail North American factory video standard. */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = AV_STANDARD_NTSC_M | AV_FLAGS_60Hz;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;



    case XC_AUDIO:
        /* Stereo + Dolby Digital enabled (0x00000001 = stereo, 0x00010000 = AC3) */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0x00010001;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_PARENTAL_CONTROL:
        /* No parental controls */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_DVD_REGION:
        /* Region 1 (North America) */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_MISC:
        /* Misc flags: 0 = no special flags */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_MAX_OS: {
        ULONG settings[24];
        if (Type) *Type = 3; /* REG_BINARY */
        if (ResultLength) *ResultLength = sizeof(settings);
        if (ValueLength < sizeof(settings))
            return (NTSTATUS)0xC0000023L; /* STATUS_BUFFER_TOO_SMALL */
        xbox_get_user_settings(settings);
        memcpy(Value, settings, sizeof(settings));
        break;
    }

    case XC_TIMEZONE_BIAS:
        if (Type) *Type = 4; /* REG_DWORD */
        if (ResultLength) *ResultLength = sizeof(LONG);
        if (ValueLength < sizeof(LONG))
            return (NTSTATUS)0xC0000023L; /* STATUS_BUFFER_TOO_SMALL */
        *(PLONG)Value = xbox_current_timezone_bias();
        break;

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
            "ExQueryNonVolatileSetting: unhandled index 0x%02X", ValueIndex);
        memset(Value, 0, ValueLength);
        if (Type) *Type = 4;
        if (ResultLength) *ResultLength = ValueLength;
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    (void)Type;
    (void)Value;
    (void)ValueLength;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX,
        "ExSaveNonVolatileSetting: index=0x%02X len=%u (ignored - read-only on PC)",
        ValueIndex, ValueLength);

    /* Accept writes silently but don't persist them */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Network / PHY
 *
 * PhyGetLinkState reports Ethernet link status. Xbox had a built-in 100Mbit
 * NIC. For PC, we report link-up since we'll handle networking differently.
 * ============================================================================ */

ULONG __stdcall xbox_PhyGetLinkState(BOOLEAN Verify)
{
    (void)Verify;
    /* Return link up, 100 Mbps, full duplex */
    return 0x01; /* XNET_ETHERNET_LINK_ACTIVE */
}

NTSTATUS __stdcall xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Param2)
{
    (void)ForceReset;
    (void)Param2;
    return STATUS_SUCCESS;
}
