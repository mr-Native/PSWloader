#!/usr/bin/env python3
"""Execute bootstrap CPU handoff under TCG; does not emulate PS5 or boot Windows."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import socket
import time
import hashlib
import subprocess
import tempfile
import zlib
from efi_acpi_observer import observe

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("bootstrap_builder", HERE / "build-bootstrap.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def main(output, payload=None, acpi_arena=None):
    output = Path(output).resolve()
    manifest = builder.build(output, HERE.parent / "evidence/windows-platform-live-20261102.json", acpi_arena)
    with tempfile.TemporaryDirectory(prefix="ps5-cpu-probe-") as directory:
        work = Path(directory)
        def assemble(name, source):
            src, binary = work / (name + ".asm"), work / (name + ".bin")
            src.write_text(source)
            subprocess.run(["nasm", "-f", "bin", str(src), "-o", str(binary)], check=True)
            return binary
        rom = assemble("rom", f"""
org 0xf0000
bits 16
start:
 cli
 cld
 mov ax,0xf000
 mov ds,ax
 lgdt [gdt_pointer-$$]
 mov eax,cr0
 or eax,1
 mov cr0,eax
 jmp dword 8:protected
bits 32
protected:
 mov ax,16
 mov ds,ax
 mov es,ax
 mov ss,ax
 mov esp,0x80000
 mov eax,0x620
 mov cr4,eax
 mov eax,{manifest['symbols']['ps5_pml4']}
 mov cr3,eax
 mov ecx,0xc0000080
 rdmsr
 or eax,0x100
 wrmsr
 mov eax,cr0
 or eax,0x80000000
 mov cr0,eax
 jmp 24:long_mode
bits 64
long_mode:
 mov esi,0x10000
 mov eax,0x100200
 jmp rax
align 8
gdt:
 dq 0,0x00cf9a000000ffff,0x00cf92000000ffff,0x00af9a000000ffff
gdt_pointer:
 dw 31
 dd gdt
times 0xfff0-($-$$) db 0
bits 16
 jmp 0xf000:0
times 0x10000-($-$$) db 0
""")
        probe = assemble("probe", """
bits 64
 cmp rcx,0x40000000
 jne failure
 cmp word [rcx],1
 jne failure
 mov rax,rsp
 and eax,15
 cmp eax,8
 jne failure
 mov qword [rsp+8],0x1234
 mov eax,0x2a
 jmp exit
failure:
 mov eax,0x2b
exit:
 mov dx,0xf4
 out dx,eax
 cli
halt:
 hlt
 jmp halt
""").read_bytes()
        # Synthetic payload: executable probe plus a checksum-valid FV header.
        # No UEFI firmware or Windows code is executed in this test.
        body = bytearray(8192)
        body[:len(probe)] = probe
        struct.pack_into("<Q", body, 4096 + 32, 4096)
        body[4096 + 40:4096 + 44] = b"_FVH"
        struct.pack_into("<H", body, 4096 + 48, 56)
        checksum = sum(struct.unpack_from("<28H", body, 4096))
        struct.pack_into("<H", body, 4096 + 50, -checksum & 0xffff)
        header = bytearray(4096)
        struct.pack_into("<8sIIQQQII", header, 0, b"PS5UPL01", 1, 4096,
                         0x41000000, len(body), 0x41000000, 1, zlib.crc32(body))
        struct.pack_into("<16sQQ", header, 48, b"uefi_fv", 0x41001000, 4096)
        struct.pack_into("<I", header, 4092, zlib.crc32(header[:4092]))
        container = work / "payload.bin"
        container.write_bytes(Path(payload).read_bytes() if payload else header + body)
        bp = bytearray(4096)
        struct.pack_into("<I", bp, 0x23c, 5)
        struct.pack_into("<I", bp, 0x218, 0x20000)
        struct.pack_into("<I", bp, 0xc0, 1)
        struct.pack_into("<I", bp, 0x21c, container.stat().st_size)
        bp[0x1e8] = 4
        struct.pack_into("<QQI", bp, 0x2d0, 0x100000, 0x7ff00000, 1)
        struct.pack_into("<QQI", bp, 0x2e4, 0x100000000, 0x80000000, 1)
        struct.pack_into("<QQI", bp, 0x2f8, 0x80000000, 0x44400000, 2)
        struct.pack_into("<QQI", bp, 0x30c, 0xd0000000, 0x10700000, 2)
        bootparams = work / "bootparams.bin"
        bootparams.write_bytes(bp)
        command = ["qemu-system-x86_64", "-machine", "pc,accel=tcg", "-cpu", "max",
                   "-m", "6G", "-smp", "1", "-display", "none",
                   "-serial", f"file:{output / 'serial.log'}" if payload else "none",
                   "-monitor", "none", "-nodefaults", "-no-reboot", "-bios", str(rom),
                   "-device", "isa-debug-exit,iobase=0xf4,iosize=4"]
        for file, address in ((output / "bootstrap-body.bin", 0x100000),
                              (bootparams, 0x10000), (container, 0x100020000)):
            command += ["-device", f"loader,file={file},addr={address},force-raw=on"]
        if payload:
            # Observation only: the real firmware is expected to remain running
            # or halt. Never interpret reaching a timeout as a successful boot.
            qmp_path = work / "qmp.sock"
            command += ["-qmp", f"unix:{qmp_path},server=on,wait=off", "-S"]
            with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                try:
                    deadline = time.monotonic() + 5
                    while not qmp_path.exists():
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError("QEMU observation endpoint unavailable")
                        time.sleep(.02)
                    with socket.socket(socket.AF_UNIX) as sock:
                        sock.settimeout(5)
                        sock.connect(str(qmp_path))
                        stream = sock.makefile("rwb")
                        json.loads(stream.readline())
                        def qmp(execute, arguments=None):
                            request = {"execute": execute}
                            if arguments: request["arguments"] = arguments
                            stream.write((json.dumps(request) + "\n").encode())
                            stream.flush()
                            while True:
                                response = json.loads(stream.readline())
                                if "error" in response: raise RuntimeError(response)
                                if "return" in response: return response["return"]
                        qmp("qmp_capabilities")
                        qmp("cont")
                        time.sleep(3)
                        status = qmp("query-status")
                        qmp("stop")
                        registers = qmp("human-monitor-command", {"command-line": "info registers"})
                        dump = work / "checkpoint.bin"
                        qmp("pmemsave", {"val": 0x111000000, "size": 40, "filename": str(dump)})
                        ring_dump = work / "firmware-ring.bin"
                        qmp("pmemsave", {"val": 0x111001000, "size": 65552, "filename": str(ring_dump)})
                        ring = ring_dump.read_bytes()
                        magic, count = struct.unpack_from("<QQ", ring)
                        firmware_log = ""
                        if magic == 0x31474f4c355350:
                            data = ring[16:]
                            if count > len(data):
                                index = count % len(data)
                                data = data[index:] + data[:index]
                            else:
                                data = data[:count]
                            firmware_log = data.decode("utf-8", errors="replace")
                        (output / "firmware.log").write_text(firmware_log)
                        evidence = {"scope": "Real UEFI payload under TCG with TEST-ONLY captured PCI profile, not PS5 hardware or Windows boot",
                                    "payload_sha256": hashlib.sha256(container.read_bytes()).hexdigest(),
                                    "status_before_stop": status, "registers": registers,
                                    "firmware_log": firmware_log,
                                    "checkpoint_qwords": list(struct.unpack("<5Q", dump.read_bytes())),
                                    "bootstrap": manifest["files"]}
                        (output / "uefi-observation.json").write_text(json.dumps(evidence, indent=2) + "\n")
                        if acpi_arena:
                            # Read after stop: no torn firmware structures. This fixture's
                            # DXE allocator uses the top of its low 2 GiB RAM window.
                            def physical_read(address, size):
                                if address < 0 or size < 0 or size > 64*1024*1024 or address + size > 0x180000000:
                                    raise ValueError("Observation read outside test RAM bounds")
                                memory = work / "acpi-memory.bin"
                                qmp("pmemsave", {"val": address, "size": size, "filename": str(memory)})
                                return memory.read_bytes()
                            published = observe(physical_read, 0x7c000000, 64*1024*1024)
                            (output / "published-acpi.json").write_text(json.dumps(published, indent=2) + "\n")
                        qmp("quit")
                        process.communicate(timeout=5)
                        print(json.dumps({"observation": str(output / "uefi-observation.json"),
                                          "checkpoint": evidence["checkpoint_qwords"],
                                          "firmware_log_bytes": len(firmware_log.encode())}, indent=2))
                        return
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()
        result = subprocess.run(command, capture_output=True, timeout=20)
        evidence = {"scope": "TCG CPU handoff with synthetic payload and TEST-ONLY captured PCI profile; no PS5 hardware, UEFI or Windows execution",
                    "qemu": subprocess.check_output(["qemu-system-x86_64", "--version"], text=True).splitlines()[0],
                    "exit_code": result.returncode, "expected_exit_code": 85,
                    "stderr": result.stderr.decode(), "bootstrap": manifest['files'],
                    "checks": ["long-mode entry and new CR3", "container and HOB preparation",
                               "copy to physical destination", "Microsoft x64 RCX argument",
                               "HOB pointer/type", "stack alignment and shadow space"]}
        (output / "execution-test.json").write_text(json.dumps(evidence, indent=2) + "\n")
        if result.returncode != 85:
            raise RuntimeError(f"CPU handoff failed: {evidence}")
        print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--payload", type=Path, help="Observe real container instead of asserting synthetic probe success")
    parser.add_argument("--acpi-arena", type=Path)
    args = parser.parse_args()
    main(args.output, args.payload, args.acpi_arena)
