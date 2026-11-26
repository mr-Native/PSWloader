#!/usr/bin/env python3
"""Host-side WDM IRP simulation unit tests for ps5m2flt.c (N5 Windows DiskDrive filter driver)."""
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT_DIR = os.path.dirname(HERE)
sys.path[:0] = [HERE, LAYOUT_DIR]   # ps5_m2_layout.py lives next to this file in the repo
import ps5_m2_layout as L

GIB = 1024 ** 3

MOCK_WDK_C = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef void VOID, *PVOID;
typedef uint8_t UCHAR, *PUCHAR, BOOLEAN;
typedef uint32_t ULONG, *PULONG;
typedef int64_t LONGLONG;
typedef int32_t NTSTATUS;
typedef uintptr_t ULONG_PTR;

#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#define STATUS_PENDING                   ((NTSTATUS)0x00000103L)
#define STATUS_MORE_PROCESSING_REQUIRED  ((NTSTATUS)0xC0000016L)
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009AL)
#define STATUS_MEDIA_WRITE_PROTECTED     ((NTSTATUS)0xC00000A2L)
#define STATUS_NO_SUCH_DEVICE            ((NTSTATUS)0xC000000EL)

#define IRP_MJ_CREATE           0x00
#define IRP_MJ_READ             0x03
#define IRP_MJ_WRITE            0x04
#define IRP_MJ_DEVICE_CONTROL   0x0E
#define IRP_MJ_POWER            0x16
#define IRP_MJ_PNP              0x1B
#define IRP_MJ_MAXIMUM_FUNCTION 0x1B

#define IRP_MN_START_DEVICE     0x00
#define IRP_MN_REMOVE_DEVICE    0x02

#define IOCTL_DISK_GET_LENGTH_INFO 0x0007405CL
#define FILE_DEVICE_DISK        0x00000007
#define FILE_DEVICE_SECURE_OPEN 0x00000100
#define DO_BUFFERED_IO          0x00000004
#define DO_DIRECT_IO            0x00000010
#define DO_DEVICE_INITIALIZING  0x00000080
#define DO_POWER_PAGABLE        0x00002000

#define IO_NO_INCREMENT         0
#define IO_DISK_INCREMENT       1
#define POOL_FLAG_NON_PAGED     1ULL
#define NormalPagePriority      16
#define MdlMappingNoExecute     0x40000000
#define DPFLTR_IHVDRIVER_ID     77
#define DPFLTR_INFO_LEVEL       3
#define DPFLTR_ERROR_LEVEL      0
#define Executive               0
#define KernelMode              0
#define NotificationEvent       0
#define UNREFERENCED_PARAMETER(x) (void)(x)

typedef union _LARGE_INTEGER {
  struct { uint32_t LowPart; int32_t HighPart; };
  LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct _GET_LENGTH_INFORMATION {
  LARGE_INTEGER Length;
} GET_LENGTH_INFORMATION;

typedef struct _IO_STATUS_BLOCK {
  NTSTATUS Status;
  ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _KEVENT { int dummy; } KEVENT, *PKEVENT;
typedef struct _IO_REMOVE_LOCK { int removed; } IO_REMOVE_LOCK, *PIO_REMOVE_LOCK;
typedef struct _UNICODE_STRING { int dummy; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct _MDL { PVOID Va; } MDL, *PMDL;

struct _DRIVER_OBJECT;
struct _DEVICE_OBJECT;
struct _IRP;
typedef NTSTATUS (*PDRIVER_DISPATCH)(struct _DEVICE_OBJECT *, struct _IRP *);
typedef NTSTATUS (*PDRIVER_ADD_DEVICE)(struct _DRIVER_OBJECT *, struct _DEVICE_OBJECT *);
typedef VOID (*PDRIVER_UNLOAD)(struct _DRIVER_OBJECT *);
typedef NTSTATUS (*PIO_COMPLETION_ROUTINE)(struct _DEVICE_OBJECT *, struct _IRP *, PVOID);

typedef struct _DRIVER_EXTENSION {
  PDRIVER_ADD_DEVICE AddDevice;
} DRIVER_EXTENSION, *PDRIVER_EXTENSION;

typedef struct _DRIVER_OBJECT {
  PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
  PDRIVER_EXTENSION DriverExtension;
  PDRIVER_UNLOAD DriverUnload;
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _DEVICE_OBJECT {
  PDRIVER_OBJECT DriverObject;
  PVOID DeviceExtension;
  ULONG Flags;
  ULONG DeviceType;
  ULONG Characteristics;
  struct _DEVICE_OBJECT *AttachedTo;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef struct _IO_STACK_LOCATION {
  UCHAR MajorFunction;
  UCHAR MinorFunction;
  union {
    struct { ULONG Length; LARGE_INTEGER ByteOffset; } Read;
    struct { ULONG Length; LARGE_INTEGER ByteOffset; } Write;
    struct { ULONG OutputBufferLength; ULONG InputBufferLength; ULONG IoControlCode; } DeviceIoControl;
  } Parameters;
  PIO_COMPLETION_ROUTINE CompletionRoutine;
  PVOID Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

typedef struct _IRP {
  IO_STATUS_BLOCK IoStatus;
  PMDL MdlAddress;
  struct { PVOID SystemBuffer; } AssociatedIrp;
  BOOLEAN PendingReturned;
  IO_STACK_LOCATION Stack[2];
  int CurrentIdx;
} IRP, *PIRP;

typedef struct _KMUTEX { int dummy; } KMUTEX, *PKMUTEX;
#define PASSIVE_LEVEL 0
static int KeGetCurrentIrql(void) { return PASSIVE_LEVEL; }
static void KeInitializeMutex(PKMUTEX m, ULONG l) { (void)m; (void)l; }
static int32_t KeReleaseMutex(PKMUTEX m, BOOLEAN w) { (void)m; (void)w; return 0; }
static void RtlZeroMemory(PVOID d, size_t n) { memset(d, 0, n); }
static PVOID ExAllocatePool2(uint64_t f, size_t n, ULONG t) { (void)f; (void)t; return calloc(1, n); }
static void ExFreePoolWithTag(PVOID p, ULONG t) { (void)t; free(p); }
static void KeInitializeEvent(PKEVENT e, int t, BOOLEAN s) { (void)e; (void)t; (void)s; }
static void KeSetEvent(PKEVENT e, int i, BOOLEAN w) { (void)e; (void)i; (void)w; }
static NTSTATUS KeWaitForSingleObject(PVOID o, int r, int m, BOOLEAN a, PVOID t) {
  (void)o; (void)r; (void)m; (void)a; (void)t; return STATUS_SUCCESS;
}
static void DbgPrintEx(ULONG id, ULONG lvl, const char *fmt, ...) { (void)id; (void)lvl; (void)fmt; }
static PVOID MmGetSystemAddressForMdlSafe(PMDL m, ULONG p) { (void)p; return m ? m->Va : NULL; }
static void IoInitializeRemoveLock(PIO_REMOVE_LOCK l, ULONG t, ULONG m, ULONG h) {
  (void)t; (void)m; (void)h; l->removed = 0;
}
static NTSTATUS IoAcquireRemoveLock(PIO_REMOVE_LOCK l, PVOID tag) {
  (void)tag; return l->removed ? STATUS_NO_SUCH_DEVICE : STATUS_SUCCESS;
}
static void IoReleaseRemoveLock(PIO_REMOVE_LOCK l, PVOID tag) { (void)l; (void)tag; }
static void IoReleaseRemoveLockAndWait(PIO_REMOVE_LOCK l, PVOID tag) { (void)tag; l->removed = 1; }
static PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { return &irp->Stack[irp->CurrentIdx]; }
static void IoSkipCurrentIrpStackLocation(PIRP irp) { (void)irp; }
static void IoCopyCurrentIrpStackLocationToNext(PIRP irp) { irp->Stack[1] = irp->Stack[0]; }
static void IoSetCompletionRoutine(PIRP irp, PIO_COMPLETION_ROUTINE cr, PVOID ctx, BOOLEAN s, BOOLEAN e, BOOLEAN c) {
  (void)s; (void)e; (void)c;
  irp->Stack[1].CompletionRoutine = cr;
  irp->Stack[1].Context = ctx;
}
static void IoMarkIrpPending(PIRP irp) { (void)irp; }
static void IoCompleteRequest(PIRP irp, int prio) { (void)irp; (void)prio; }
static void PoStartNextPowerIrp(PIRP irp) { (void)irp; }

/* Simulated lower disk state */
static uint64_t g_disk_total_sectors = 0;
static uint8_t  g_lba0[512];
static uint8_t  g_nested_head34[34 * 512];
static uint8_t  g_nested_backup[512];
static int      g_has_nested_gpt = 0;
static uint64_t g_lower_read_calls = 0;
static uint64_t g_lower_write_calls = 0;
static uint64_t g_lower_ioctl_calls = 0;
static uint8_t  g_fill_byte = 0x5A;

static NTSTATUS IoCallDriver(PDEVICE_OBJECT dev, PIRP irp) {
  (void)dev;
  PIO_STACK_LOCATION sp = &irp->Stack[0];
  if (sp->MajorFunction == IRP_MJ_PNP) {
    irp->IoStatus.Status = STATUS_SUCCESS;
  } else if (sp->MajorFunction == IRP_MJ_DEVICE_CONTROL &&
             sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_DISK_GET_LENGTH_INFO) {
    GET_LENGTH_INFORMATION *li = (GET_LENGTH_INFORMATION *)irp->AssociatedIrp.SystemBuffer;
    li->Length.QuadPart = (LONGLONG)(g_disk_total_sectors * 512ULL);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = sizeof(*li);
  } else if (sp->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
    g_lower_ioctl_calls++;
    irp->IoStatus.Status = STATUS_SUCCESS;
  } else if (sp->MajorFunction == IRP_MJ_READ) {
    g_lower_read_calls++;
    uint64_t lba = (uint64_t)sp->Parameters.Read.ByteOffset.QuadPart / 512ULL;
    uint32_t len = sp->Parameters.Read.Length;
    uint8_t *dst = irp->MdlAddress ? (uint8_t *)irp->MdlAddress->Va : (uint8_t *)irp->AssociatedIrp.SystemBuffer;
    memset(dst, g_fill_byte, len);
    if (lba == 0ULL && len == 512U) {
      memcpy(dst, g_lba0, 512U);
    } else if (g_has_nested_gpt) {
      if (lba == 65536ULL && len == sizeof(g_nested_head34)) {
        memcpy(dst, g_nested_head34, len);
      } else if (lba == (g_disk_total_sectors - 1ULL - 34224ULL) && len == 512U) {
        memcpy(dst, g_nested_backup, 512U);
      }
    }
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = len;
  } else if (sp->MajorFunction == IRP_MJ_WRITE) {
    g_lower_write_calls++;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = sp->Parameters.Write.Length;
  } else {
    irp->IoStatus.Status = STATUS_SUCCESS;
  }

  if (irp->Stack[1].CompletionRoutine) {
    PIO_COMPLETION_ROUTINE cr = irp->Stack[1].CompletionRoutine;
    PVOID ctx = irp->Stack[1].Context;
    irp->Stack[1].CompletionRoutine = NULL;
    cr(dev, irp, ctx);
  }
  return irp->IoStatus.Status;
}

static NTSTATUS PoCallDriver(PDEVICE_OBJECT dev, PIRP irp) { return IoCallDriver(dev, irp); }

static PIRP IoBuildDeviceIoControlRequest(ULONG code, PDEVICE_OBJECT dev, PVOID inb, ULONG inl,
                                          PVOID outb, ULONG outl, BOOLEAN internal, PKEVENT ev, PIO_STATUS_BLOCK ios) {
  (void)dev; (void)inb; (void)inl; (void)internal; (void)ev;
  static IRP s_irp;
  memset(&s_irp, 0, sizeof(s_irp));
  s_irp.Stack[0].MajorFunction = IRP_MJ_DEVICE_CONTROL;
  s_irp.Stack[0].Parameters.DeviceIoControl.IoControlCode = code;
  s_irp.Stack[0].Parameters.DeviceIoControl.OutputBufferLength = outl;
  s_irp.AssociatedIrp.SystemBuffer = outb;
  ios->Status = STATUS_SUCCESS;
  return &s_irp;
}

static PIRP IoBuildSynchronousFsdRequest(ULONG mj, PDEVICE_OBJECT dev, PVOID buf, ULONG len,
                                         LARGE_INTEGER *off, PKEVENT ev, PIO_STATUS_BLOCK ios) {
  (void)dev; (void)ev;
  static IRP s_irp;
  memset(&s_irp, 0, sizeof(s_irp));
  s_irp.Stack[0].MajorFunction = (UCHAR)mj;
  s_irp.Stack[0].Parameters.Read.Length = len;
  s_irp.Stack[0].Parameters.Read.ByteOffset = *off;
  s_irp.AssociatedIrp.SystemBuffer = buf;
  ios->Status = STATUS_SUCCESS;
  return &s_irp;
}

static PDEVICE_OBJECT g_created_fdo = NULL;
static DEVICE_OBJECT g_lower_dev;
static DRIVER_EXTENSION g_drv_ext;
static DRIVER_OBJECT g_drv_obj;

static NTSTATUS IoCreateDevice(PDRIVER_OBJECT drv, ULONG ext_sz, PUNICODE_STRING name, ULONG type,
                               ULONG chars, BOOLEAN excl, PDEVICE_OBJECT *out) {
  (void)name; (void)excl;
  PDEVICE_OBJECT d = (PDEVICE_OBJECT)calloc(1, sizeof(DEVICE_OBJECT));
  d->DriverObject = drv;
  d->DeviceExtension = calloc(1, ext_sz);
  d->DeviceType = type;
  d->Characteristics = chars;
  d->Flags = DO_DEVICE_INITIALIZING;
  *out = d;
  g_created_fdo = d;
  return STATUS_SUCCESS;
}

static PDEVICE_OBJECT IoAttachDeviceToDeviceStack(PDEVICE_OBJECT src, PDEVICE_OBJECT tgt) {
  src->AttachedTo = tgt;
  return tgt;
}
static void IoDetachDevice(PDEVICE_OBJECT tgt) { (void)tgt; }
static void IoDeleteDevice(PDEVICE_OBJECT d) {
  if (d) { free(d->DeviceExtension); free(d); }
  if (g_created_fdo == d) g_created_fdo = NULL;
}
"""


class TestPs5M2FltWdm(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.mkdtemp(prefix="ps5m2flt-wdm-")
        with open(os.path.join(cls.build_dir, "ntddk.h"), "w", encoding="utf-8") as f:
            f.write(MOCK_WDK_C)
        with open(os.path.join(cls.build_dir, "ntdddisk.h"), "w", encoding="utf-8") as f:
            f.write("/* mocked in ntddk.h */\n")

        harness_c = os.path.join(cls.build_dir, "harness.c")
        with open(harness_c, "w", encoding="utf-8") as f:
            f.write(
                """
#include "ps5m2flt.c"

int h_init_and_start(uint64_t total_sectors, const uint8_t *lba0, const uint8_t *head34, const uint8_t *backup_hdr, int has_nested) {
  if (g_created_fdo) {
    IoDeleteDevice(g_created_fdo);
  }
  memset(&g_drv_obj, 0, sizeof(g_drv_obj));
  memset(&g_drv_ext, 0, sizeof(g_drv_ext));
  g_drv_obj.DriverExtension = &g_drv_ext;
  memset(&g_lower_dev, 0, sizeof(g_lower_dev));
  g_lower_dev.Flags = DO_DIRECT_IO;
  g_lower_dev.DeviceType = FILE_DEVICE_DISK;

  g_disk_total_sectors = total_sectors;
  g_has_nested_gpt = has_nested;
  memset(g_lba0, 0, sizeof(g_lba0));
  if (lba0) {
    memcpy(g_lba0, lba0, sizeof(g_lba0));
  } else {
    g_lba0[510] = 0x55;
    g_lba0[511] = 0xAA;
  }
  if (has_nested && head34 && backup_hdr) {
    memcpy(g_nested_head34, head34, sizeof(g_nested_head34));
    memcpy(g_nested_backup, backup_hdr, sizeof(g_nested_backup));
  }
  g_lower_read_calls = 0;
  g_lower_write_calls = 0;
  g_lower_ioctl_calls = 0;

  DriverEntry(&g_drv_obj, NULL);
  if (!NT_SUCCESS(g_drv_ext.AddDevice(&g_drv_obj, &g_lower_dev))) return -1;

  IRP start_irp;
  memset(&start_irp, 0, sizeof(start_irp));
  start_irp.Stack[0].MajorFunction = IRP_MJ_PNP;
  start_irp.Stack[0].MinorFunction = IRP_MN_START_DEVICE;
  if (!NT_SUCCESS(g_drv_obj.MajorFunction[IRP_MJ_PNP](g_created_fdo, &start_irp))) return -2;

  g_lower_read_calls = 0;
  g_lower_write_calls = 0;
  g_lower_ioctl_calls = 0;
  PPS5_M2_DEVEXT ext = (PPS5_M2_DEVEXT)g_created_fdo->DeviceExtension;
  if (ext->Overlay.Valid) return 1;
  if (ext->FailSafeWriteProtect) return 2;
  return 0;
}

int32_t h_read_sectors(uint64_t lba, uint32_t sectors, uint8_t *buf, uint64_t *out_lower_reads) {
  IRP irp;
  MDL mdl;
  memset(&irp, 0, sizeof(irp));
  mdl.Va = buf;
  irp.MdlAddress = &mdl;
  irp.Stack[0].MajorFunction = IRP_MJ_READ;
  irp.Stack[0].Parameters.Read.ByteOffset.QuadPart = (LONGLONG)(lba * 512ULL);
  irp.Stack[0].Parameters.Read.Length = sectors * 512U;
  uint64_t before = g_lower_read_calls;
  NTSTATUS st = g_drv_obj.MajorFunction[IRP_MJ_READ](g_created_fdo, &irp);
  *out_lower_reads = g_lower_read_calls - before;
  return st;
}

int32_t h_write_sectors(uint64_t lba, uint32_t sectors, uint64_t *out_lower_writes) {
  IRP irp;
  memset(&irp, 0, sizeof(irp));
  uint8_t *buf = (uint8_t *)calloc(sectors ? sectors : 1, 512U);
  irp.AssociatedIrp.SystemBuffer = buf;
  irp.Stack[0].MajorFunction = IRP_MJ_WRITE;
  irp.Stack[0].Parameters.Write.ByteOffset.QuadPart = (LONGLONG)(lba * 512ULL);
  irp.Stack[0].Parameters.Write.Length = sectors * 512U;
  uint64_t before = g_lower_write_calls;
  NTSTATUS st = g_drv_obj.MajorFunction[IRP_MJ_WRITE](g_created_fdo, &irp);
  *out_lower_writes = g_lower_write_calls - before;
  free(buf);
  return st;
}

int32_t h_ioctl(uint32_t code, const uint8_t *in_buf, uint32_t in_len, uint64_t *out_lower_ioctls) {
  IRP irp;
  memset(&irp, 0, sizeof(irp));
  irp.Stack[0].MajorFunction = IRP_MJ_DEVICE_CONTROL;
  irp.Stack[0].Parameters.DeviceIoControl.IoControlCode = code;
  irp.Stack[0].Parameters.DeviceIoControl.InputBufferLength = in_len;
  irp.AssociatedIrp.SystemBuffer = (PVOID)in_buf;
  uint64_t before = g_lower_ioctl_calls;
  NTSTATUS st = g_drv_obj.MajorFunction[IRP_MJ_DEVICE_CONTROL](g_created_fdo, &irp);
  *out_lower_ioctls = g_lower_ioctl_calls - before;
  return st;
}
"""
            )
        so_path = os.path.join(cls.build_dir, "libps5m2flt_test.so")
        subprocess.run(
            [
                "gcc",
                "-shared",
                "-fPIC",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-multichar",
                "-I",
                cls.build_dir,
                "-I",
                HERE,
                "-I",
                LAYOUT_DIR,
                "-I",
                os.path.join(HERE, "..", "..", "firmware"),   # ps5-m2-overlay.h
                harness_c,
                "-o",
                so_path,
            ],
            check=True,
        )
        cls.lib = ctypes.CDLL(so_path)
        cls.lib.h_init_and_start.argtypes = [ctypes.c_uint64, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        cls.lib.h_init_and_start.restype = ctypes.c_int
        cls.lib.h_read_sectors.argtypes = [
            ctypes.c_uint64,
            ctypes.c_uint32,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_uint64),
        ]
        cls.lib.h_read_sectors.restype = ctypes.c_int32
        cls.lib.h_write_sectors.argtypes = [
            ctypes.c_uint64,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint64),
        ]
        cls.lib.h_write_sectors.restype = ctypes.c_int32
        cls.lib.h_ioctl.argtypes = [
            ctypes.c_uint32,
            ctypes.c_char_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint64),
        ]
        cls.lib.h_ioctl.restype = ctypes.c_int32

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.build_dir, ignore_errors=True)

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="ps5m2flt-img-")
        self.img = os.path.join(self.d, "m2.img")
        with open(self.img, "wb") as f:
            f.truncate(8 * GIB)
        self.disk = L.Disk(self.img, writable=True)
        parts, fu, lu = L.plan(self.disk.n, esp_mib=260)
        L.write_nested(self.disk, parts, fu, lu)
        self.disk.f.flush()
        self.head34 = self.disk.read(L.SONY_HEAD, 34)
        self.backup_hdr = self.disk.read(self.disk.last_lba, 1)
        self.py_sec, _ = L.overlay_sectors(self.disk)

    def tearDown(self):
        self.disk.f.close()
        shutil.rmtree(self.d, ignore_errors=True)

    def test_wdm_ps5_m2_full_irp_lifecycle(self):
        gameos_lba0 = b"\x30\x6b" + b"\x9F" * 510
        active = self.lib.h_init_and_start(self.disk.n, gameos_lba0, self.head34, self.backup_hdr, 1)
        self.assertEqual(active, 1)

        lower_calls = ctypes.c_uint64(0)

        # 1. Pure synthetic read of LBA 0..33: must return synthetic GPT with 0 lower driver calls
        buf34 = ctypes.create_string_buffer(34 * 512)
        st = self.lib.h_read_sectors(0, 34, buf34, ctypes.byref(lower_calls))
        self.assertEqual(st, 0)
        self.assertEqual(lower_calls.value, 0)
        for i in range(34):
            sec = buf34.raw[i * 512 : (i + 1) * 512]
            if i == 0:
                self.assertEqual(sec, self.py_sec[0])
            elif i == 1:
                self.assertEqual(sec[:8], b"EFI PART")
                fu = int.from_bytes(sec[40:48], "little")
                lu = int.from_bytes(sec[48:56], "little")
                self.assertEqual(fu, 34)
                self.assertEqual(lu, self.disk.n - 34)
            elif i == 2:
                # Real nested partitions come first at Entry 0.. so Windows partition numbers match UEFI 1:1
                self.assertIn("PS5M2ESP".encode("utf-16le"), sec[0:128])
            elif i == 33:
                # Slots 126 & 127 (entries 2 & 3 of LBA 33) = PS5 GameOS Header & Tail guard partitions
                self.assertIn("PS5 GameOS Header".encode("utf-16le"), sec[256:384])
                self.assertIn("PS5 GameOS Tail".encode("utf-16le"), sec[384:512])

        # 2. Pure synthetic read of LBA 30..39: 0 lower driver calls, LBA 34..39 = cleanly zeroed (no fake FAT16)
        buf10 = ctypes.create_string_buffer(b"\xFF" * (10 * 512), 10 * 512)
        st = self.lib.h_read_sectors(30, 10, buf10, ctypes.byref(lower_calls))
        self.assertEqual(st, 0)
        self.assertEqual(lower_calls.value, 0)
        for i in range(4, 10):
            self.assertEqual(buf10.raw[i * 512 : (i + 1) * 512], b"\x00" * 512)

        # 3. Boundary-crossing read at LBA 67580..67589 (NestedFirstUsable = 67584): 1 lower call, 67580..67583 zeroed, 67584..67589 passthrough
        buf_b = ctypes.create_string_buffer(10 * 512)
        st = self.lib.h_read_sectors(L.SONY_HEAD + 2048 - 4, 10, buf_b, ctypes.byref(lower_calls))
        self.assertEqual(st, 0)
        self.assertEqual(lower_calls.value, 1)
        for i in range(4):
            self.assertEqual(buf_b.raw[i * 512 : (i + 1) * 512], b"\x00" * 512)
        for i in range(4, 10):
            self.assertEqual(buf_b.raw[i * 512 : (i + 1) * 512], b"\x5A" * 512)

        # 4. Normal partition read (LBA 65536 + 2048): fast path (1 lower call, untouched 0x5A data)
        buf_norm = ctypes.create_string_buffer(8 * 512)
        st = self.lib.h_read_sectors(L.SONY_HEAD + 2048, 8, buf_norm, ctypes.byref(lower_calls))
        self.assertEqual(st, 0)
        self.assertEqual(lower_calls.value, 1)
        self.assertEqual(buf_norm.raw, b"\x5A" * (8 * 512))

        # 5. Write guard: writes to GameOS head/tail (outside synthetic GPT) blocked with STATUS_MEDIA_WRITE_PROTECTED
        STATUS_MEDIA_WRITE_PROTECTED = ctypes.c_int32(0xC00000A2).value
        for bad_lba, cnt in [(34, 1), (L.SONY_HEAD - 1, 1), (L.SONY_HEAD - 1, 2), (self.disk.last_lba, 2), (self.disk.last_lba + 1, 1)]:
            st = self.lib.h_write_sectors(bad_lba, cnt, ctypes.byref(lower_calls))
            self.assertEqual(st, STATUS_MEDIA_WRITE_PROTECTED)
            self.assertEqual(lower_calls.value, 0)

        # 5b. Attempting to wipe LBA 1..33 (deleting GameOS guard partitions) is BLOCKED with STATUS_MEDIA_WRITE_PROTECTED
        st = self.lib.h_write_sectors(1, 33, ctypes.byref(lower_calls))
        self.assertEqual(st, STATUS_MEDIA_WRITE_PROTECTED)
        self.assertEqual(lower_calls.value, 0)

        # 5c. Writing LBA 0 (PMBR) safely redirects to nested GPT without touching real LBA 0
        st = self.lib.h_write_sectors(0, 1, ctypes.byref(lower_calls))
        self.assertEqual(st, 0)
        self.assertEqual(lower_calls.value, 2)

        # 6. Allowed writes inside [65536, last_lba] pass through to lower driver
        for ok_lba, cnt in [(L.SONY_HEAD, 1), (L.SONY_HEAD + 4096, 64), (self.disk.last_lba, 1)]:
            st = self.lib.h_write_sectors(ok_lba, cnt, ctypes.byref(lower_calls))
            self.assertEqual(st, 0)
            self.assertEqual(lower_calls.value, 1)

        # 7. Hardening #1: Destructive / passthrough IOCTLs blocked, safe TRIM allowed, overlapping TRIM blocked
        for blocked_ioctl in (0x0004D004, 0x0004D014, 0x0004D02C, 0x002DD3C0, 0x002D5190, 0x0007C054, 0x0007C058):
            st = self.lib.h_ioctl(blocked_ioctl, None, 0, ctypes.byref(lower_calls))
            self.assertEqual(st, STATUS_MEDIA_WRITE_PROTECTED)
            self.assertEqual(lower_calls.value, 0)

        import struct
        # Safe TRIM inside [65536, last_lba] (DataSetRangesOffset = 32, DataSetRangesLength = 16)
        dsm_ok = struct.pack("<8IqQ", 28, 1, 0, 0, 0, 32, 16, 0, (L.SONY_HEAD + 2048) * 512, 4096 * 512)
        st = self.lib.h_ioctl(0x002D9404, dsm_ok, len(dsm_ok), ctypes.byref(lower_calls))
        self.assertEqual(st, 0)
        self.assertEqual(lower_calls.value, 1)

        # Unsafe TRIM overlapping LBA 0..65535
        dsm_bad = struct.pack("<8IqQ", 28, 1, 0, 0, 0, 32, 16, 0, 0, 4096 * 512)
        st = self.lib.h_ioctl(0x002D9404, dsm_bad, len(dsm_bad), ctypes.byref(lower_calls))
        self.assertEqual(st, STATUS_MEDIA_WRITE_PROTECTED)
        self.assertEqual(lower_calls.value, 0)

    def test_wdm_failsafe_on_unformatted_gameos_m2_vs_usb_passthrough(self):
        STATUS_MEDIA_WRITE_PROTECTED = ctypes.c_int32(0xC00000A2).value
        lower_calls = ctypes.c_uint64(0)

        # 1. Fresh GameOS-formatted M.2 without nested GPT (LBA 0 has encrypted GameOS header, no 0x55AA) -> empty writable nested GPT (mode 1)
        gameos_lba0 = b"\x30\x6b" + b"\x9F" * 510
        mode = self.lib.h_init_and_start(self.disk.n, gameos_lba0, None, None, 0)
        self.assertEqual(mode, 1)  # empty writable nested GPT overlay
        # GameOS head (LBA 34..65535) is still strictly blocked
        self.assertEqual(self.lib.h_write_sectors(34, 1, ctypes.byref(lower_calls)), STATUS_MEDIA_WRITE_PROTECTED)
        self.assertEqual(lower_calls.value, 0)

        # 1b. Hardening #3: Corrupted nested GPT at LBA 65536 -> FailSafeWriteProtect (mode 2)
        bad_nested = bytearray(34 * 512)
        bad_nested[512:520] = b"EFI PART"  # valid sig, invalid CRC
        mode_bad = self.lib.h_init_and_start(self.disk.n, gameos_lba0, bytes(bad_nested), b"\x00" * 512, 1)
        self.assertEqual(mode_bad, 2)  # FailSafeWriteProtect == TRUE
        self.assertEqual(self.lib.h_write_sectors(L.SONY_HEAD + 2048, 8, ctypes.byref(lower_calls)), STATUS_MEDIA_WRITE_PROTECTED)
        self.assertEqual(lower_calls.value, 0)
        self.assertEqual(self.lib.h_ioctl(0x00070024, None, 0, ctypes.byref(lower_calls)), STATUS_MEDIA_WRITE_PROTECTED)

        # 2. Standard USB boot stick (LBA 0 has 0x55AA signature, no nested GPT at 65536) -> passthrough (mode 0)
        usb_lba0 = b"\x00" * 510 + b"\x55\xAA"
        mode_usb = self.lib.h_init_and_start(125000000, usb_lba0, None, None, 0)
        self.assertEqual(mode_usb, 0)
        self.assertEqual(self.lib.h_write_sectors(0, 1, ctypes.byref(lower_calls)), 0)
        self.assertEqual(lower_calls.value, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
