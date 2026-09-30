/*
 * kernel_io.c - I/O manager compatibility stubs
 *
 * Device objects, IRP management, and completion helpers do not implement
 * the Xbox driver stack. Host file operations are handled by kernel_file.c
 * and the guest bridge. Translated XDK code still calls these entrypoints.
 */

#include "kernel.h"
#include <string.h>

/* ============================================================================
 * Type object pointers
 *
 * Exported compatibility objects for device, completion-port, and file types.
 * ============================================================================ */

static ULONG g_device_object_type_data     = 0x44455643; /* 'DEVC' */
static ULONG g_completion_object_type_data = 0x434F4D50; /* 'COMP' */
static ULONG g_file_object_type_data       = 0x46494C45; /* 'FILE' */

PVOID xbox_IoDeviceObjectType     = &g_device_object_type_data;
PVOID xbox_IoCompletionObjectType = &g_completion_object_type_data;
PVOID xbox_IoFileObjectType       = &g_file_object_type_data;

/* ============================================================================
 * Device Management
 *
 * IoCreateDevice/IoDeleteDevice create kernel device objects. Since we
 * don't have real kernel drivers, we allocate dummy objects that satisfy
 * the interface without actual device functionality.
 * ============================================================================ */

/* Minimal device object - just enough to not crash if someone reads fields */
typedef struct _XBOX_FAKE_DEVICE {
    ULONG Type;
    ULONG Size;
    PVOID DeviceExtension;
    PVOID DriverObject;
} XBOX_FAKE_DEVICE;

NTSTATUS __stdcall xbox_IoCreateDevice(
    PVOID DriverObject,
    ULONG DeviceExtensionSize,
    PXBOX_ANSI_STRING DeviceName,
    ULONG DeviceType,
    BOOLEAN Exclusive,
    PVOID* DeviceObject)
{
    XBOX_FAKE_DEVICE* device;
    ULONG total_size;

    (void)DriverObject;
    (void)DeviceType;
    (void)Exclusive;

    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;

    total_size = sizeof(XBOX_FAKE_DEVICE) + DeviceExtensionSize;
    device = (XBOX_FAKE_DEVICE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_size);
    if (!device) {
        *DeviceObject = NULL;
        return STATUS_NO_MEMORY;
    }

    device->Type = DeviceType;
    device->Size = total_size;
    device->DriverObject = DriverObject;
    device->DeviceExtension = (DeviceExtensionSize > 0)
        ? (PVOID)((UCHAR*)device + sizeof(XBOX_FAKE_DEVICE))
        : NULL;

    *DeviceObject = device;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
        "IoCreateDevice: '%.*s' type=%u ext_size=%u → %p",
        DeviceName ? DeviceName->Length : 0,
        DeviceName ? DeviceName->Buffer : "<null>",
        DeviceType, DeviceExtensionSize, device);

    return STATUS_SUCCESS;
}

VOID __stdcall xbox_IoDeleteDevice(PVOID DeviceObject)
{
    if (DeviceObject) {
        xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
            "IoDeleteDevice: %p", DeviceObject);
        HeapFree(GetProcessHeap(), 0, DeviceObject);
    }
}

/* ============================================================================
 * IRP Management Stubs
 *
 * IRPs (I/O Request Packets) are the core mechanism for driver I/O on Xbox.
 * Since we don't use real drivers, these are all stubs.
 * ============================================================================ */

VOID __stdcall xbox_IoInitializeIrp(PVOID Irp, USHORT PacketSize, CCHAR StackSize)
{
    (void)StackSize;
    if (Irp)
        memset(Irp, 0, PacketSize);
}

VOID __stdcall xbox_IoStartNextPacket(PVOID DeviceObject, BOOLEAN Cancelable)
{
    (void)DeviceObject;
    (void)Cancelable;
}

VOID __stdcall xbox_IoStartNextPacketByKey(PVOID DeviceObject, BOOLEAN Cancelable, ULONG Key)
{
    (void)DeviceObject;
    (void)Cancelable;
    (void)Key;
}

VOID __stdcall xbox_IoStartPacket(PVOID DeviceObject, PVOID Irp, PULONG Key, PVOID CancelFunction)
{
    (void)DeviceObject;
    (void)Irp;
    (void)Key;
    (void)CancelFunction;
}

VOID __stdcall xbox_IoMarkIrpMustComplete(PVOID Irp)
{
    (void)Irp;
}

/* ============================================================================
 * I/O Completion Ports
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoSetIoCompletion(
    PVOID IoCompletion,
    PVOID KeyContext,
    PVOID ApcContext,
    NTSTATUS IoStatus,
    ULONG_PTR IoStatusInformation)
{
    (void)IoCompletion;
    (void)KeyContext;
    (void)ApcContext;
    (void)IoStatus;
    (void)IoStatusInformation;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSetIoCompletion: completion=%p status=0x%08X (stubbed)",
        IoCompletion, IoStatus);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Synchronous device I/O compatibility stubs
 *
 * These helpers report success without executing a driver request; output
 * length/status fields are initialized where supplied.
 * ============================================================================ */

NTSTATUS __stdcall xbox_IoSynchronousDeviceIoControlRequest(
    ULONG IoControlCode,
    PVOID DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PULONG ReturnedOutputBufferLength,
    BOOLEAN InternalDeviceIoControl)
{
    (void)IoControlCode;
    (void)DeviceObject;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    (void)InternalDeviceIoControl;

    if (ReturnedOutputBufferLength)
        *ReturnedOutputBufferLength = 0;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSynchronousDeviceIoControlRequest: ioctl=0x%08X device=%p (stubbed)",
        IoControlCode, DeviceObject);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode,
    PVOID DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    HANDLE Event,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    (void)IoControlCode;
    (void)DeviceObject;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    (void)InternalDeviceIoControl;
    (void)Event;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_IoSynchronousFsdRequest(
    ULONG MajorFunction,
    PVOID DeviceObject,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER StartingOffset)
{
    (void)MajorFunction;
    (void)DeviceObject;
    (void)Buffer;
    (void)Length;
    (void)StartingOffset;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IoSynchronousFsdRequest: major=%u device=%p (stubbed)",
        MajorFunction, DeviceObject);

    return STATUS_SUCCESS;
}

PVOID __stdcall xbox_IoBuildSynchronousFsdRequest(
    ULONG MajorFunction,
    PVOID DeviceObject,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER StartingOffset,
    HANDLE Event,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    (void)DeviceObject;
    (void)Buffer;
    (void)Length;
    (void)StartingOffset;
    (void)Event;

    /*
     * Returns a built IRP for the caller to hand to IofCallDriver. We have no
     * driver stack, so there is nothing meaningful to build. Returning NULL is
     * the documented allocation-failure path, which callers already handle;
     * inventing a fake IRP would only push the failure to whoever dereferences
     * it. IoStatusBlock is filled in so a caller that reads it without checking
     * the return sees a coherent result.
     */
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_IO,
        "IoBuildSynchronousFsdRequest: major=%u len=%u - no driver stack, returning NULL",
        MajorFunction, Length);

    return NULL;
}

/* ============================================================================
 * Driver Dispatch
 *
 * IofCallDriver/IofCompleteRequest are __fastcall on Xbox (the 'f' suffix).
 * Getting the convention wrong here corrupts the stack at every call site.
 * ============================================================================ */

NTSTATUS __fastcall xbox_IofCallDriver(PVOID DeviceObject, PVOID Irp)
{
    (void)DeviceObject;
    (void)Irp;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_IO,
        "IofCallDriver: device=%p irp=%p (stubbed)", DeviceObject, Irp);

    return STATUS_SUCCESS;
}

VOID __fastcall xbox_IofCompleteRequest(PVOID Irp, CCHAR PriorityBoost)
{
    (void)Irp;
    (void)PriorityBoost;
}

NTSTATUS __stdcall xbox_IoInvalidDeviceRequest(PVOID DeviceObject, PVOID Irp)
{
    (void)DeviceObject;
    (void)Irp;

    /*
     * The default dispatch entry for major functions a driver does not handle.
     * Its whole contract is to reject, so this is a real implementation rather
     * than a stub.
     */
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ============================================================================
 * Symbolic Links
 *
 * Games create links like \??\D: -> \Device\Cdrom0. Actual path translation is
 * table-driven in kernel_path.c, which already knows the standard Xbox drive
 * letters, so these only need to record the mapping and stay consistent:
 * creating a duplicate must collide, and deleting an unknown link must fail.
 * ============================================================================ */

#define XBOX_MAX_SYMLINKS 32

typedef struct _XBOX_SYMLINK {
    char link[64];
    char target[128];
    BOOL used;
} XBOX_SYMLINK;

static XBOX_SYMLINK g_symlinks[XBOX_MAX_SYMLINKS];

/* Copy a counted, non-NUL-terminated ANSI_STRING into a C buffer. */
static void xbox_copy_ansi(char* dst, size_t dst_size, PXBOX_ANSI_STRING src)
{
    size_t n;

    if (!dst || dst_size == 0)
        return;
    dst[0] = '\0';
    if (!src || !src->Buffer)
        return;

    n = (size_t)src->Length;
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, src->Buffer, n);
    dst[n] = '\0';
}

static XBOX_SYMLINK* xbox_find_symlink(const char* name)
{
    int i;
    for (i = 0; i < XBOX_MAX_SYMLINKS; i++) {
        if (g_symlinks[i].used && strcmp(g_symlinks[i].link, name) == 0)
            return &g_symlinks[i];
    }
    return NULL;
}

NTSTATUS __stdcall xbox_IoCreateSymbolicLink(
    PXBOX_ANSI_STRING SymbolicLinkName,
    PXBOX_ANSI_STRING DeviceName)
{
    char link[64], target[128];
    int i;

    if (!SymbolicLinkName)
        return STATUS_INVALID_PARAMETER;

    xbox_copy_ansi(link, sizeof(link), SymbolicLinkName);
    xbox_copy_ansi(target, sizeof(target), DeviceName);

    if (link[0] == '\0')
        return STATUS_INVALID_PARAMETER;

    if (xbox_find_symlink(link)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_IO,
            "IoCreateSymbolicLink: '%s' already exists", link);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    for (i = 0; i < XBOX_MAX_SYMLINKS; i++) {
        if (!g_symlinks[i].used) {
            g_symlinks[i].used = TRUE;
            strcpy(g_symlinks[i].link, link);
            strcpy(g_symlinks[i].target, target);
            xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO,
                "IoCreateSymbolicLink: '%s' -> '%s'", link, target);
            return STATUS_SUCCESS;
        }
    }

    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_IO,
        "IoCreateSymbolicLink: table full (%d), dropping '%s'",
        XBOX_MAX_SYMLINKS, link);
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS __stdcall xbox_IoDeleteSymbolicLink(PXBOX_ANSI_STRING SymbolicLinkName)
{
    char link[64];
    XBOX_SYMLINK* entry;

    if (!SymbolicLinkName)
        return STATUS_INVALID_PARAMETER;

    xbox_copy_ansi(link, sizeof(link), SymbolicLinkName);
    entry = xbox_find_symlink(link);
    if (!entry) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_IO,
            "IoDeleteSymbolicLink: '%s' not found", link);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_IO, "IoDeleteSymbolicLink: '%s'", link);
    entry->used = FALSE;
    entry->link[0] = '\0';
    entry->target[0] = '\0';
    return STATUS_SUCCESS;
}
