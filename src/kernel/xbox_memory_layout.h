/**
 * Xbox memory layout compatibility.
 *
 * Section addresses are parsed from the XBE. The stack, heap, and
 * synthetic kernel layout still need to match the supported title.
 * Windows uses shared file-mapping views for guest RAM and its aliases:
 * 64 MiB normally, or 128 MiB with the optional PC graphics bank.
 * All XBE sections are copied at their guest VAs within this mapping.
 * Host addresses are obtained by adding the guest-memory offset.
 */

#ifndef XBOX_MEMORY_LAYOUT_H
#define XBOX_MEMORY_LAYOUT_H

#include "platform/xbox_winnt.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 * ================================================================ */

/* Base address of all XBE files in Xbox memory */
#define XBOX_BASE_ADDRESS       0x00010000

/* Start of mapped region - includes low memory (KPCR at 0x0) because
 * game code reads from addresses like 0x20 and 0x28 (Xbox kernel structures). */
#define XBOX_MAP_START          0x00000000

/* Xbox physical memory */
#define XBOX_TOTAL_RAM          (64 * 1024 * 1024)  /* 64 MB */
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/* Section addresses are parsed from the XBE header by
 * xbox_MemoryLayoutInit(); this does not remove title-specific layout
 * requirements for the stack, heap, or synthetic kernel data. */

/* ================================================================
 * Memory initialization
 * ================================================================ */

/**
 * Initialize shared guest RAM, its aliases, and the title memory layout.
 * Copies every XBE section, including .text, and zero-fills virtual tails.
 * The retail .rdata/.data boundary shares a host page and remains writable.
 * Guest VAs must be translated using xbox_GetMemoryOffset().
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file.
 * @return TRUE on success, FALSE on failure.
 */
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);

/* Failure stage and Windows error from the last initialization attempt. */
const char *xbox_GetMemoryLayoutError(void);

/**
 * Release the reserved Xbox memory layout.
 */
void xbox_MemoryLayoutShutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_IsXboxAddress(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory layout is not initialized.
 */
void *xbox_GetMemoryBase(void);

/* Optional PC graphics bank for large mods. Configure before layout init.
 * Game objects and the kernel heap stay in the retail lower 64 MiB. */
void xbox_SetExtendedGraphicsMemory(BOOL enabled);
uint32_t xbox_GetGraphicsMemorySize(void);
uint32_t xbox_GetExtendedGraphicsPool(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);

/* ================================================================
 * Xbox stack for recompiled code
 * ================================================================ */

/* ================================================================
 * Kernel data export area
 * ================================================================ */

/** Base VA for kernel data exports (XboxHardwareInfo, XboxKrnlVersion, etc.)
 *  These are kernel exports that are DATA, not functions. The game reads
 *  their thunk entries and dereferences them to access the data. */
#define XBOX_KERNEL_DATA_BASE   0x0087E000
#define XBOX_KERNEL_DATA_SIZE   4096   /* 4 KB - plenty for all data exports */

/* Offsets within the kernel data area */
#define KDATA_HARDWARE_INFO     0x000  /* XBOX_HARDWARE_INFO (8 bytes) */
#define KDATA_KRNL_VERSION      0x010  /* XBOX_KRNL_VERSION (8 bytes) */
#define KDATA_TICK_COUNT        0x020  /* KeTickCount (4 bytes) */
#define KDATA_LAUNCH_DATA_PAGE  0x030  /* LaunchDataPage (4 bytes, pointer) */
#define KDATA_THREAD_OBJ_TYPE   0x040  /* PsThreadObjectType (4 bytes) */
#define KDATA_EVENT_OBJ_TYPE    0x050  /* ExEventObjectType (4 bytes) */
#define KDATA_XE_IMAGE_FILENAME 0x060  /* XeImageFileName (ANSI_STRING) */
#define KDATA_IO_COMPLETION_TYPE 0x070 /* IoCompletionObjectType (4 bytes) */
#define KDATA_IO_DEVICE_TYPE    0x080  /* IoDeviceObjectType (4 bytes) */
#define KDATA_IO_FILE_TYPE      0x090  /* IoFileObjectType (4 bytes) */
#define KDATA_HAL_CACHE_PARTITIONS 0x094 /* HalDiskCachePartitionCount */
#define KDATA_HAL_BOOT_VIDEO_MODE 0x098 /* HalBootSMCVideoMode */
#define KDATA_IDEX_CHANNEL_OBJECT 0x09C /* IdexChannelObject pointer */
#define KDATA_EEPROM_KEY        0x0A0  /* XboxEEPROMKey (16 bytes) */
#define KDATA_IDEX_CHANNEL_DATA 0x0B0  /* dummy IDE channel object */
#define KDATA_HD_KEY            0x100  /* XboxHDKey (16 bytes) */
#define KDATA_SIGNATURE_KEY     0x110  /* XboxSignatureKey (16 bytes) */
#define KDATA_LAN_KEY           0x120  /* XboxLANKey (16 bytes) */
#define KDATA_ALT_SIGNATURE_KEYS 0x130 /* XboxAlternateSignatureKeys (256 bytes) */
#define KDATA_XE_PUBLIC_KEY     0x300  /* XePublicKeyData (284 bytes) */
#define KDATA_XE_IMAGE_BUFFER   0x500  /* XeImageFileName string buffer */

/** Size of the simulated Xbox stack (256 KiB, from the retail Mercenaries
 * XBE PEStackCommit field at header offset 0x130).
 * Translated native C frames use the host stack; this guest range only holds
 * Xbox-visible call frames, TLS/SEH data, and game locals. */
#define XBOX_STACK_SIZE     (256 * 1024)

/** Base VA of the stack area (above last XBE section). */
#define XBOX_STACK_BASE     0x00880000

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

/* ================================================================
 * Xbox dynamic heap (for MmAllocateContiguousMemory, etc.)
 * ================================================================ */

/** Base VA of the dynamic heap area (above stack). */
#define XBOX_HEAP_BASE      (XBOX_STACK_BASE + XBOX_STACK_SIZE)

/** Size of the dynamic heap.
 *  Xbox has 64 MB total RAM. The total mapped region (data + stack + heap)
 *  must equal 64 MB so the RenderWare engine's memory probing stops at the
 *  correct boundary. On a real Xbox, probing past 64 MB causes a page fault
 *  that the engine catches via SEH to determine available memory. */
#define XBOX_HEAP_SIZE      (XBOX_TOTAL_RAM - XBOX_HEAP_BASE)

/** No static mirror/guard region. RAM mirror is handled via file mapping
 *  views that alias the same physical pages as the base 64 MB region. */
#define XBOX_MIRROR_SIZE    0
#define XBOX_GUARD_SIZE     0

/** Number of 64 MB mirror views to pre-map (covers 1.75 GB of address space). */
#define XBOX_NUM_MIRRORS    28

/**
 * Allocate from the Xbox heap. Returns an Xbox VA, or 0 on failure.
 * Alignment must be a power of 2 (minimum 4).
 * Caller must serialize access; this allocator has no internal lock.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/** Allocate complete 4 KiB guest pages while retaining the caller's required
 * base alignment. Xbox physical/contiguous allocations own every page they
 * touch; sharing an unused tail with pool memory corrupts both allocators. */
uint32_t xbox_HeapAllocPageRounded(uint32_t size, uint32_t alignment);
/** Requested byte count recorded for an allocation beginning at xbox_va. */
uint32_t xbox_HeapGetRequestedSize(uint32_t xbox_va);

/** Reserved byte count tracked for the allocation. */
uint32_t xbox_HeapGetAllocationSize(uint32_t xbox_va);


/** Free a tracked block so a later compatible allocation can reuse it. */
void xbox_HeapFree(uint32_t xbox_va);

/**
 * Get the file mapping handle for the Xbox memory region.
 * Used by the VEH handler to map additional mirror views on demand.
 * Returns NULL if file mapping is not available.
 */
HANDLE xbox_GetMappingHandle(void);

/** Map one 64 KiB block of the Xbox direct physical-memory windows
 * (KSEG0/KSEG1, 0x80000000-0xBFFFFFFF) as an alias of the 64 MiB RAM
 * backing store.  The MCPX exposes only 26 physical address bits, so the
 * whole 1 GiB CPU aperture repeats the installed 64 MiB every 0x04000000.
 * Intended for demand mapping from the access-violation handler.
 */
BOOL xbox_MapPhysicalAlias(uint32_t xbox_va);


#ifdef __cplusplus
}
#endif

#endif /* XBOX_MEMORY_LAYOUT_H */
