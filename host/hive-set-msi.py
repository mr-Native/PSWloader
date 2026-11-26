#!/usr/bin/env python3
"""v1.0 Offline-enable MSI for one PCI device instance in a Windows SYSTEM hive.

PS5 native Windows has no _PRT/INTx routing, so devices whose inbox INF does not
request MSI (e.g. HDA 1002:13EA with hdaudbus.inf) fail with code 12. This sets
  <ControlSetXXX>\\Enum\\PCI\\<hwid>\\<instance>\\Device Parameters\\
      Interrupt Management\\MessageSignaledInterruptProperties\\MSISupported = 1 (REG_DWORD)
for the current control set only. Input hive is never modified; writes OUTPUT.

usage: hive-set-msi.py SYSTEM_IN SYSTEM_OUT 'PCI\\VEN_1002&DEV_13EA&...\\4&XXXXXXXX&0&0141'
"""
import struct
import sys

import hivex

DWORD = 4


def child(h, node, name):
    for c in h.node_children(node):
        if h.node_name(c).lower() == name.lower():
            return c
    return None


def ensure(h, node, name):
    c = child(h, node, name)
    if c is None:
        c = h.node_add_child(node, name)
    return c


def current_control_set(h):
    sel = child(h, h.root(), 'Select')
    val = h.node_get_value(sel, 'Current')
    return 'ControlSet%03d' % h.value_dword(val)


def set_msi(src, dst, instance):
    h = hivex.Hivex(src, write=True)
    parts = instance.split('\\')
    if len(parts) != 3 or parts[0].upper() != 'PCI':
        raise SystemExit('instance must be PCI\\<hwid>\\<instance>')
    node = child(h, h.root(), current_control_set(h))
    for name in ('Enum', 'PCI', parts[1], parts[2]):
        node = child(h, node, name)
        if node is None:
            raise SystemExit(f'missing key {name} for {instance}')
    for name in ('Device Parameters', 'Interrupt Management', 'MessageSignaledInterruptProperties'):
        node = ensure(h, node, name)
    h.node_set_value(node, {'key': 'MSISupported', 't': DWORD, 'value': struct.pack('<I', 1)})
    h.commit(dst)
    return verify(dst, instance)


def verify(path, instance):
    h = hivex.Hivex(path)
    parts = instance.split('\\')
    node = child(h, h.root(), current_control_set(h))
    for name in ('Enum', 'PCI', parts[1], parts[2], 'Device Parameters', 'Interrupt Management',
                 'MessageSignaledInterruptProperties'):
        node = child(h, node, name)
        if node is None:
            return False
    return h.value_dword(h.node_get_value(node, 'MSISupported')) == 1


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] == sys.argv[2]:
        raise SystemExit(__doc__)
    ok = set_msi(sys.argv[1], sys.argv[2], sys.argv[3])
    print('MSISupported=1 verified' if ok else 'verification FAILED')
    sys.exit(0 if ok else 1)
