/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * pswdisk v1.2 (17.11.2026, PSWloader; v1.2: USB disks in UAS mode (uaspstor, a Storport miniport) report BusTypeUsb in
 * the ADAPTER descriptor - a disk is now taken when its device OR its adapter descriptor says USB, and every storage
 * adapter with BusTypeUsb (GUID_DEVINTERFACE_STORAGEPORT, \\.\ScsiN:) gets a filter too that rewrites
 * StorageAdapterProperty; the setup stick's adapter is left alone (parent device of the excluded disk); counters per
 * way: RewrittenDisk, RewrittenAdapter; v1.1: readable device-independent name "USB: <vendor> <product>" - the device
 * descriptor is rebuilt (pswdisk-desc.h), header-only and short buffers get the new Size; the setup stick is left alone
 * (REG_DWORD ExcludeDisk = its disk number, written by psw-drivers.cmd before the start)) - lets Windows treat USB disks as fixed internal disks, so the stock Windows 11
 * Setup accepts a USB SSD as installation target ("Windows can't be installed on drives connected via USB or IEEE 1394").
 *
 * How: a non-PnP kernel service. At start it registers for disk interface arrivals (GUID_DEVINTERFACE_DISK, existing
 * ones included). For every disk whose device descriptor reports BusTypeUsb it attaches one filter device on top of
 * that disk's stack. The filter passes everything through and only rewrites answers on the way up:
 *   IOCTL_STORAGE_QUERY_PROPERTY  StorageDeviceProperty  : BusType Usb -> Sata, RemovableMedia -> FALSE
 *                                   + product "USB: <vendor> <product>", vendor string folded into it (v1.1)
                                 StorageAdapterProperty : BusType Usb -> Sata
 *   IOCTL_STORAGE_GET_HOTPLUG_INFO                       : MediaRemovable, MediaHotplug, DeviceHotplug -> FALSE
 * Nothing on the disk is changed, no data is read or written by the driver. Disks behind other buses are not touched.
 * Removal: IRP_MN_REMOVE_DEVICE detaches and deletes the filter. Counters for the log go to the service
 * key (REG_DWORD Attached, AttachedAdapter, Excluded, ExcludedAdapter, RewrittenDisk, RewrittenAdapter, Rewritten = sum).
 * Load in WinPE (Windows Setup): sc create pswdisk type= kernel start= demand binPath= X:\PSWDRV\pswdisk.sys
 *                                sc start pswdisk          (test signing must be on: PSWloader firmware r5.22+)
 */
#include <ntddk.h>
#include <ntddstor.h>
#include <initguid.h>
#include <ntdddisk.h>
#include <wdmguid.h>
#include <devpkey.h>
#include "pswdisk-desc.h"
NTKERNELAPI PDEVICE_OBJECT IoGetDeviceAttachmentBaseRef(PDEVICE_OBJECT DeviceObject);   /* ntifs.h; exported by ntoskrnl */

#define PSWDISK_VERSION 0x00010002u
C_ASSERT(FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) == PSWDISK_HDR);
C_ASSERT(FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, BusType) == 28 && FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, ProductIdOffset) == 16);
C_ASSERT(BusTypeUsb == PSWDISK_BUS_USB && BusTypeSata == PSWDISK_BUS_SATA);
#define QBUF 1024u
#define TAG 'ksDP'

#define INST_MAX 200u   /* WCHARs of a device instance id */
typedef struct {
    PDEVICE_OBJECT Lower;
    PDEVICE_OBJECT Self;
    BOOLEAN        Removed;
    BOOLEAN        Adapter;              /* v1.2: filter on a storage adapter (\\.\ScsiN:), not on a disk */
    WCHAR          Inst[INST_MAX];       /* v1.2: adapter only - instance id of the adapter's device (base of its stack) */
} FILTER_EXT;

DRIVER_INITIALIZE DriverEntry;
static PDRIVER_OBJECT g_Driver;
static PVOID g_Notify, g_NotifyAdapter;
static UNICODE_STRING g_RegPath;
static WCHAR g_RegBuf[260];
static volatile LONG g_Attached, g_AttachedAdapter, g_RewrittenDisk, g_RewrittenAdapter, g_Excluded, g_ExcludedAdapter;
static LONG g_NotedDisk = -1, g_NotedAdapter = -1, g_NotedExAd = -1;
static ULONG g_ExcludeDisk = 0xFFFFFFFFu;
static WCHAR g_ExcludeParent[INST_MAX];      /* v1.2: parent device (= adapter device) of the excluded disk, "" = none */
static PVOID volatile g_Anchor = (PVOID)DriverEntry;   /* forces one DIR64 base relocation (driver toolchain note) */

static VOID note(PCWSTR name, LONG value)
{
    ULONG v = (ULONG)value;
    if (g_RegPath.Length) (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, g_RegPath.Buffer, name, REG_DWORD, &v, sizeof v);
}

/* ---------------------------------------------------------------- rewriting on completion */
static BOOLEAN is_fdo_ours(PDEVICE_OBJECT d) { return d && d->DriverObject == g_Driver; }

#define CTX_ADAPTER 0x100u
static VOID count(ULONG ctx) { InterlockedIncrement((ctx & CTX_ADAPTER) ? &g_RewrittenAdapter : &g_RewrittenDisk); }

static NTSTATUS complete_query(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    ULONG prop = (ULONG)(ULONG_PTR)ctx & 0xFFu;
    UNREFERENCED_PARAMETER(dev);
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    if (NT_SUCCESS(irp->IoStatus.Status) && irp->AssociatedIrp.SystemBuffer) {
        ULONG_PTR n = irp->IoStatus.Information;
        if (prop == StorageDeviceProperty && n >= RTL_SIZEOF_THROUGH_FIELD(STORAGE_DEVICE_DESCRIPTOR, BusType)) {
            PSTORAGE_DEVICE_DESCRIPTOR d = (PSTORAGE_DEVICE_DESCRIPTOR)irp->AssociatedIrp.SystemBuffer;
            if (d->BusType == BusTypeUsb) { d->BusType = BusTypeSata; d->RemovableMedia = FALSE; count((ULONG)(ULONG_PTR)ctx); }
        } else if (prop == StorageAdapterProperty && n >= RTL_SIZEOF_THROUGH_FIELD(STORAGE_ADAPTER_DESCRIPTOR, BusType)) {
            PSTORAGE_ADAPTER_DESCRIPTOR a = (PSTORAGE_ADAPTER_DESCRIPTOR)irp->AssociatedIrp.SystemBuffer;
            if (a->BusType == BusTypeUsb) { a->BusType = BusTypeSata; count((ULONG)(ULONG_PTR)ctx); }
        }
    }
    return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS complete_hotplug(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(dev); UNREFERENCED_PARAMETER(ctx);
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    if (NT_SUCCESS(irp->IoStatus.Status) && irp->AssociatedIrp.SystemBuffer && irp->IoStatus.Information >= sizeof(STORAGE_HOTPLUG_INFO)) {
        PSTORAGE_HOTPLUG_INFO h = (PSTORAGE_HOTPLUG_INFO)irp->AssociatedIrp.SystemBuffer;
        h->MediaRemovable = FALSE; h->MediaHotplug = FALSE; h->DeviceHotplug = FALSE;
        count(0);
    }
    return STATUS_CONTINUE_COMPLETION;
}

/* synchronous IOCTL to a device (PASSIVE_LEVEL); returns bytes in *got */
static NTSTATUS sync_ioctl(PDEVICE_OBJECT dev, ULONG code, PVOID in, ULONG inlen, PVOID out, ULONG outlen, ULONG_PTR *got)
{
    IO_STATUS_BLOCK iosb; KEVENT ev; PIRP irp; NTSTATUS st;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(code, dev, in, inlen, out, outlen, FALSE, &ev, &iosb);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    st = IoCallDriver(dev, irp);
    if (st == STATUS_PENDING) { KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL); st = iosb.Status; }
    *got = NT_SUCCESS(st) ? iosb.Information : 0;
    return st;
}

/* v1.1: StorageDeviceProperty answered by the filter itself: full query below, rebuilt descriptor, copied up to the
 * caller's length (Information = copied bytes, Size = the whole new descriptor, as the class driver does).
 * FALSE = not handled (caller passes the IRP down with the completion-time patch). */
static BOOLEAN answer_device_property(FILTER_EXT *x, PIRP irp, ULONG outlen)
{
    STORAGE_PROPERTY_QUERY q; PUCHAR raw, neu; ULONG_PTR got = 0; ULONG need, n;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || outlen < sizeof(STORAGE_DESCRIPTOR_HEADER)) return FALSE;
    raw = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, QBUF, TAG);
    neu = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, pswdisk_desc_bound(QBUF), TAG);
    if (!raw || !neu) { if (raw) ExFreePoolWithTag(raw, TAG); if (neu) ExFreePoolWithTag(neu, TAG); return FALSE; }
    RtlZeroMemory(&q, sizeof q); q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
    need = 0;
    if (NT_SUCCESS(sync_ioctl(x->Lower, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, raw, QBUF, &got)))
        need = (ULONG)pswdisk_desc_rebuild(raw, (unsigned long)got, neu, pswdisk_desc_bound(QBUF));
    if (need) {
        n = need < outlen ? need : outlen;
        RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, neu, n);
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = n;
        count(0);
    }
    ExFreePoolWithTag(raw, TAG); ExFreePoolWithTag(neu, TAG);
    if (!need) return FALSE;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return TRUE;
}

/* ---------------------------------------------------------------- dispatch */
static NTSTATUS pass(PDEVICE_OBJECT dev, PIRP irp)
{
    FILTER_EXT *x = (FILTER_EXT *)dev->DeviceExtension;
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(x->Lower, irp);
}

/* v1.2: counters to the service key, from dispatch at PASSIVE_LEVEL only (completion routines may run at DISPATCH) */
static VOID note_counts(VOID)
{
    LONG d = g_RewrittenDisk, a = g_RewrittenAdapter, e = g_ExcludedAdapter;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || (d == g_NotedDisk && a == g_NotedAdapter && e == g_NotedExAd)) return;
    g_NotedDisk = d; g_NotedAdapter = a; g_NotedExAd = e;
    note(L"RewrittenDisk", d); note(L"RewrittenAdapter", a); note(L"Rewritten", d + a); note(L"ExcludedAdapter", e);
}

/* v1.2: the setup stick's own adapter stays USB (decided per request: the disk may arrive after its adapter) */
static BOOLEAN adapter_excluded(FILTER_EXT *x)
{
    if (!g_ExcludeParent[0] || !x->Inst[0] || _wcsicmp(x->Inst, g_ExcludeParent) != 0) return FALSE;
    InterlockedIncrement(&g_ExcludedAdapter);
    return TRUE;
}

static NTSTATUS dispatch_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    FILTER_EXT *x = (FILTER_EXT *)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    note_counts();
    if (x->Adapter) {   /* v1.2: adapter filter - only StorageAdapterProperty, everything else (SCSI address, pass-through) unchanged */
        if (code == IOCTL_STORAGE_QUERY_PROPERTY && s->Parameters.DeviceIoControl.InputBufferLength >= sizeof(STORAGE_PROPERTY_QUERY) &&
            irp->AssociatedIrp.SystemBuffer && !adapter_excluded(x)) {
            PSTORAGE_PROPERTY_QUERY q = (PSTORAGE_PROPERTY_QUERY)irp->AssociatedIrp.SystemBuffer;
            if (q->QueryType == PropertyStandardQuery && q->PropertyId == StorageAdapterProperty) {
                IoCopyCurrentIrpStackLocationToNext(irp);
                IoSetCompletionRoutine(irp, complete_query, (PVOID)(ULONG_PTR)(StorageAdapterProperty | CTX_ADAPTER), TRUE, FALSE, FALSE);
                return IoCallDriver(x->Lower, irp);
            }
        }
        return pass(dev, irp);
    }
    if (code == IOCTL_STORAGE_QUERY_PROPERTY && s->Parameters.DeviceIoControl.InputBufferLength >= sizeof(STORAGE_PROPERTY_QUERY) &&
        irp->AssociatedIrp.SystemBuffer) {
        PSTORAGE_PROPERTY_QUERY q = (PSTORAGE_PROPERTY_QUERY)irp->AssociatedIrp.SystemBuffer;
        if (q->QueryType == PropertyStandardQuery && q->PropertyId == StorageDeviceProperty &&
            answer_device_property(x, irp, s->Parameters.DeviceIoControl.OutputBufferLength))
            return STATUS_SUCCESS;
        if (q->QueryType == PropertyStandardQuery && (q->PropertyId == StorageDeviceProperty || q->PropertyId == StorageAdapterProperty)) {
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, complete_query, (PVOID)(ULONG_PTR)q->PropertyId, TRUE, FALSE, FALSE);
            return IoCallDriver(x->Lower, irp);
        }
    } else if (code == IOCTL_STORAGE_GET_HOTPLUG_INFO) {
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, complete_hotplug, NULL, TRUE, FALSE, FALSE);
        return IoCallDriver(x->Lower, irp);
    }
    return pass(dev, irp);
}

static NTSTATUS dispatch_pnp(PDEVICE_OBJECT dev, PIRP irp)
{
    FILTER_EXT *x = (FILTER_EXT *)dev->DeviceExtension;
    UCHAR minor = IoGetCurrentIrpStackLocation(irp)->MinorFunction;
    PDEVICE_OBJECT lower = x->Lower;
    NTSTATUS st;
    IoSkipCurrentIrpStackLocation(irp);
    st = IoCallDriver(lower, irp);
    if (minor == IRP_MN_REMOVE_DEVICE && !x->Removed) {
        x->Removed = TRUE;
        IoDetachDevice(lower);
        IoDeleteDevice(dev);
        InterlockedDecrement(x->Adapter ? &g_AttachedAdapter : &g_Attached);
    }
    return st;
}

static NTSTATUS dispatch_power(PDEVICE_OBJECT dev, PIRP irp)
{
    FILTER_EXT *x = (FILTER_EXT *)dev->DeviceExtension;
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(x->Lower, irp);
}

/* ---------------------------------------------------------------- attach */
/* StorageAdapterProperty of a disk or adapter stack: BusTypeUsb? */
static BOOLEAN adapter_is_usb(PDEVICE_OBJECT top)
{
    STORAGE_PROPERTY_QUERY q; UCHAR buf[sizeof(STORAGE_ADAPTER_DESCRIPTOR) + 16]; ULONG_PTR got = 0;
    RtlZeroMemory(&q, sizeof q); RtlZeroMemory(buf, sizeof buf);
    q.PropertyId = StorageAdapterProperty; q.QueryType = PropertyStandardQuery;
    return NT_SUCCESS(sync_ioctl(top, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got)) &&
           got >= RTL_SIZEOF_THROUGH_FIELD(STORAGE_ADAPTER_DESCRIPTOR, BusType) && ((PSTORAGE_ADAPTER_DESCRIPTOR)buf)->BusType == BusTypeUsb;
}

/* StorageDeviceProperty of the top of the disk stack: is it a USB disk? v1.2: or does its adapter say USB (UAS) */
static BOOLEAN disk_is_usb(PDEVICE_OBJECT top)
{
    STORAGE_PROPERTY_QUERY q; UCHAR buf[sizeof(STORAGE_DEVICE_DESCRIPTOR) + 64]; ULONG_PTR got = 0;
    RtlZeroMemory(&q, sizeof q); RtlZeroMemory(buf, sizeof buf);
    q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
    if (NT_SUCCESS(sync_ioctl(top, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got)) &&
        got >= RTL_SIZEOF_THROUGH_FIELD(STORAGE_DEVICE_DESCRIPTOR, BusType) && ((PSTORAGE_DEVICE_DESCRIPTOR)buf)->BusType == BusTypeUsb)
        return TRUE;
    return adapter_is_usb(top);
}

/* v1.2: a string device property of the device at the base of the stack (the PDO) */
static BOOLEAN base_property(PDEVICE_OBJECT top, const DEVPROPKEY *key, WCHAR *out)
{
    PDEVICE_OBJECT base = IoGetDeviceAttachmentBaseRef(top);
    ULONG req = 0; DEVPROPTYPE type = 0; NTSTATUS st;
    out[0] = 0;
    if (!base) return FALSE;
    st = IoGetDevicePropertyData(base, key, LOCALE_NEUTRAL, 0, (INST_MAX - 1) * sizeof(WCHAR), out, &req, &type);
    ObDereferenceObject(base);
    out[INST_MAX - 1] = 0;
    if (!NT_SUCCESS(st) || type != DEVPROP_TYPE_STRING) { out[0] = 0; return FALSE; }
    return TRUE;
}

/* v1.1: the setup stick (disk number from psw-drivers.cmd) stays a USB disk */
static BOOLEAN disk_excluded(PDEVICE_OBJECT top)
{
    STORAGE_DEVICE_NUMBER dn; ULONG_PTR got = 0;
    if (g_ExcludeDisk == 0xFFFFFFFFu) return FALSE;
    RtlZeroMemory(&dn, sizeof dn);
    return NT_SUCCESS(sync_ioctl(top, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &dn, sizeof dn, &got)) && got >= sizeof dn &&
           dn.DeviceType == FILE_DEVICE_DISK && dn.DeviceNumber == g_ExcludeDisk;
}

static NTSTATUS on_disk(PVOID notification, PVOID context)
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION n = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)notification;
    PFILE_OBJECT file = NULL; PDEVICE_OBJECT top = NULL, filt = NULL, lower;
    FILTER_EXT *x;
    UNREFERENCED_PARAMETER(context);
    if (!IsEqualGUID(&n->Event, &GUID_DEVICE_INTERFACE_ARRIVAL)) return STATUS_SUCCESS;
    if (!NT_SUCCESS(IoGetDeviceObjectPointer(n->SymbolicLinkName, FILE_READ_ATTRIBUTES, &file, &top))) return STATUS_SUCCESS;
    if (is_fdo_ours(top) || !disk_is_usb(top)) { ObDereferenceObject(file); return STATUS_SUCCESS; }
    if (disk_excluded(top)) {
        WCHAR par[INST_MAX];
        if (base_property(top, &DEVPKEY_Device_Parent, par)) RtlCopyMemory(g_ExcludeParent, par, sizeof par);   /* v1.2: its adapter */
        note(L"Excluded", InterlockedIncrement(&g_Excluded)); ObDereferenceObject(file); return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(IoCreateDevice(g_Driver, sizeof(FILTER_EXT), NULL, top->DeviceType, top->Characteristics & FILE_DEVICE_SECURE_OPEN,
                                   FALSE, &filt))) { ObDereferenceObject(file); return STATUS_SUCCESS; }
    x = (FILTER_EXT *)filt->DeviceExtension; RtlZeroMemory(x, sizeof *x); x->Self = filt;
    lower = IoAttachDeviceToDeviceStack(filt, top);
    if (!lower) { IoDeleteDevice(filt); ObDereferenceObject(file); return STATUS_SUCCESS; }
    x->Lower = lower;
    filt->Flags |= lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    filt->Characteristics |= lower->Characteristics;
    filt->Flags &= ~DO_DEVICE_INITIALIZING;
    note(L"Attached", InterlockedIncrement(&g_Attached));
    ObDereferenceObject(file);
    return STATUS_SUCCESS;
}

/* v1.2: storage adapters (Storport/SCSI port, \\.\ScsiN:) whose adapter descriptor says USB - UAS (uaspstor) */
static NTSTATUS on_adapter(PVOID notification, PVOID context)
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION n = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)notification;
    PFILE_OBJECT file = NULL; PDEVICE_OBJECT top = NULL, filt = NULL, lower;
    FILTER_EXT *x;
    UNREFERENCED_PARAMETER(context);
    if (!IsEqualGUID(&n->Event, &GUID_DEVICE_INTERFACE_ARRIVAL)) return STATUS_SUCCESS;
    if (!NT_SUCCESS(IoGetDeviceObjectPointer(n->SymbolicLinkName, FILE_READ_ATTRIBUTES, &file, &top))) return STATUS_SUCCESS;
    if (is_fdo_ours(top) || !adapter_is_usb(top)) { ObDereferenceObject(file); return STATUS_SUCCESS; }
    if (!NT_SUCCESS(IoCreateDevice(g_Driver, sizeof(FILTER_EXT), NULL, top->DeviceType, top->Characteristics & FILE_DEVICE_SECURE_OPEN,
                                   FALSE, &filt))) { ObDereferenceObject(file); return STATUS_SUCCESS; }
    x = (FILTER_EXT *)filt->DeviceExtension; RtlZeroMemory(x, sizeof *x); x->Self = filt; x->Adapter = TRUE;
    (VOID)base_property(top, &DEVPKEY_Device_InstanceId, x->Inst);
    lower = IoAttachDeviceToDeviceStack(filt, top);
    if (!lower) { IoDeleteDevice(filt); ObDereferenceObject(file); return STATUS_SUCCESS; }
    x->Lower = lower;
    filt->Flags |= lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    filt->Characteristics |= lower->Characteristics;
    filt->Flags &= ~DO_DEVICE_INITIALIZING;
    note(L"AttachedAdapter", InterlockedIncrement(&g_AttachedAdapter));
    ObDereferenceObject(file);
    return STATUS_SUCCESS;
}

static VOID start_watch(PDRIVER_OBJECT drv)
{
    (VOID)IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange, PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                             (PVOID)&GUID_DEVINTERFACE_DISK, drv, on_disk, NULL, &g_Notify);
    (VOID)IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange, PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                             (PVOID)&GUID_DEVINTERFACE_STORAGEPORT, drv, on_adapter, NULL, &g_NotifyAdapter);
    note(L"Version", (LONG)PSWDISK_VERSION);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    ULONG i;
    if (!g_Anchor) return STATUS_UNSUCCESSFUL;
    g_Driver = drv;
    if (reg && reg->Length < sizeof g_RegBuf - sizeof(WCHAR)) {
        RTL_QUERY_REGISTRY_TABLE t[2]; ULONG def = 0xFFFFFFFFu;
        RtlCopyMemory(g_RegBuf, reg->Buffer, reg->Length); g_RegBuf[reg->Length / sizeof(WCHAR)] = 0;
        RtlInitUnicodeString(&g_RegPath, g_RegBuf);
        RtlZeroMemory(t, sizeof t);
        t[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
        t[0].Name = L"ExcludeDisk"; t[0].EntryContext = &g_ExcludeDisk;
        t[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD; t[0].DefaultData = &def; t[0].DefaultLength = sizeof def;
        (VOID)RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, g_RegBuf, t, NULL, NULL);
    }
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = pass;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_ioctl;
    drv->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    drv->MajorFunction[IRP_MJ_POWER] = dispatch_power;
    /* no DriverUnload: filters attached to disk stacks must stay while the disks live */
    start_watch(drv);   /* existing disks now, later ones (also at boot start) as their interfaces arrive */
    return STATUS_SUCCESS;
}
