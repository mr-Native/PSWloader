#!/usr/bin/env python3
"""v1.2 Offline edits of Windows registry hives (SOFTWARE, BCD, ...) for PS5 native Windows.

Refuses dirty hives (header sequence numbers differ = unreplayed LOG1/LOG2 data from a
power-cut Windows session). The input file is never modified; the result goes to OUTPUT.
Equal sequences alone are insufficient: checks also verify the header checksum and
read every reachable key/value before edits and after committing the local output.
This is a structural gate, not a substitute for Windows boot validation.

usage:
  hive-edit.py IN OUT set 'Key\\Path' NAME dword|sz|binary VALUE [set ...]
  hive-edit.py IN OUT bcd-testsigning        (AllowPrereleaseSignatures on the default loader)
  hive-edit.py IN OUT bcd-no-recovery        (recoveryenabled No + bootstatuspolicy IgnoreAllFailures)
  hive-edit.py IN - check                    (reject dirty/checksum-invalid/unreadable hives)
"""
import struct
import sys

import hivex

T_SZ, T_BINARY, T_DWORD = 1, 3, 4
BOOTMGR = '{9dea862c-5cdd-4e70-acc1-f32b344d4795}'
EL_DEFAULT_OBJECT = '23000003'
EL_ALLOW_PRERELEASE = '16000049'   # BcdLibraryBoolean_AllowPrereleaseSignatures ("testsigning")
EL_RECOVERY_ENABLED = '16000009'   # BcdLibraryBoolean_AutoRecoveryEnabled
EL_BOOT_STATUS_POLICY = '250000e0' # BcdOSLoaderInteger_BootStatusPolicy (1 = IgnoreAllFailures)


def sequence(path):
    with open(path, 'rb') as f:
        head = f.read(512)
    if head[:4] != b'regf':
        raise SystemExit(f'{path}: not a registry hive')
    return struct.unpack_from('<II', head, 4)


def validate_hive(path):
    """Check the header and every reachable key/value, not just the sequence pair."""
    with open(path, 'rb') as f:
        head = f.read(512)
    checksum = 0
    for offset in range(0, 508, 4):
        checksum ^= struct.unpack_from('<I', head, offset)[0]
    checksum = 0xfffffffe if checksum == 0xffffffff else 1 if checksum == 0 else checksum
    if checksum != struct.unpack_from('<I', head, 508)[0]:
        raise SystemExit(f'{path}: hive header checksum mismatch')
    keys, values, seen = 0, 0, set()
    try:
        h = hivex.Hivex(str(path))
        pending = [h.root()]
        while pending:
            node = pending.pop()
            if node in seen:
                raise RuntimeError('repeated key reference or cycle')
            seen.add(node)
            h.node_name(node)
            pending.extend(h.node_children(node))
            keys += 1
            for value in h.node_values(node):
                h.value_key(value)
                h.value_value(value)
                values += 1
    except (RuntimeError, ValueError, OSError) as error:
        raise SystemExit(f'{path}: hive structure validation failed: {error}') from error
    return keys, values


def child(h, node, name):
    for c in h.node_children(node):
        if h.node_name(c).lower() == name.lower():
            return c
    return None


def walk(h, path, create):
    node = h.root()
    for part in [p for p in path.split('\\') if p]:
        nxt = child(h, node, part)
        if nxt is None:
            if not create:
                return None
            nxt = h.node_add_child(node, part)
        node = nxt
    return node


def encode(kind, value):
    if kind == 'dword':
        return T_DWORD, struct.pack('<I', int(value, 0))
    if kind == 'sz':
        return T_SZ, (value + '\0').encode('utf-16-le')
    if kind == 'binary':
        return T_BINARY, bytes.fromhex(value)
    raise SystemExit(f'unknown type {kind}')


def set_value(h, path, name, kind, value):
    t, raw = encode(kind, value)
    h.node_set_value(walk(h, path, True), {'key': name, 't': t, 'value': raw})
    return path, name, t, raw


def get_value(h, path, name):
    node = walk(h, path, False)
    if node is None:
        return None
    try:
        v = h.node_get_value(node, name)
    except RuntimeError:
        return None
    return h.value_value(v)


def bcd_default_loader(h):
    t, raw = get_value(h, f'Objects\\{BOOTMGR}\\Elements\\{EL_DEFAULT_OBJECT}', 'Element')
    return raw.decode('utf-16-le').rstrip('\0')


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    src, dst, op = argv[0], argv[1], argv[2]
    s1, s2 = sequence(src)
    if op == 'check':
        print(f'sequence {s1} {s2}', 'clean' if s1 == s2 else 'DIRTY')
        if s1 != s2:
            return 1
        keys, values = validate_hive(src)
        print(f'structure readable: {keys} keys, {values} values')
        return 0
    if s1 != s2:
        raise SystemExit(f'{src}: dirty hive (sequence {s1}/{s2}) - boot Windows and shut it down cleanly first')
    if dst == src:
        raise SystemExit('OUTPUT must differ from INPUT')
    validate_hive(src)
    h = hivex.Hivex(src, write=True)
    wanted = []
    if op == 'bcd-testsigning':
        loader = bcd_default_loader(h)
        wanted.append(set_value(h, f'Objects\\{loader}\\Elements\\{EL_ALLOW_PRERELEASE}', 'Element', 'binary', '01'))
        print('default loader', loader)
    elif op == 'bcd-no-recovery':
        # v1.1: after hard hangs bootmgr otherwise switches to WinRE ("Please wait"), which hangs on the PS5
        loader = bcd_default_loader(h)
        wanted.append(set_value(h, f'Objects\\{loader}\\Elements\\{EL_RECOVERY_ENABLED}', 'Element', 'binary', '00'))
        wanted.append(set_value(h, f'Objects\\{loader}\\Elements\\{EL_BOOT_STATUS_POLICY}', 'Element', 'binary', '0100000000000000'))
        print('default loader', loader)
    elif op == 'set':
        args = argv[2:]
        while args:
            if args[0] != 'set' or len(args) < 5:
                raise SystemExit('expected: set PATH NAME TYPE VALUE')
            wanted.append(set_value(h, *args[1:5]))
            args = args[5:]
    else:
        raise SystemExit(f'unknown operation {op}')
    h.commit(dst)
    validate_hive(dst)
    check = hivex.Hivex(dst)
    for path, name, t, raw in wanted:
        got = get_value(check, path, name)
        if got != (t, raw):
            raise SystemExit(f'verification failed for {path}\\{name}: {got}')
        print('verified', path, name, raw.hex())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
