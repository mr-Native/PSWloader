"""Reject misleading firmware evidence before reporting publication."""
import struct
import unittest
import zlib
from efi_acpi_observer import ACPI_GUID, observe


class ObservationTests(unittest.TestCase):
    def setUp(self):
        self.memory = bytearray(4096)
        m = self.memory
        m[:8] = b'IBI SYST'
        struct.pack_into('<I', m, 12, 120)
        struct.pack_into('<QQ', m, 104, 1, 256)
        m[256:272] = ACPI_GUID
        struct.pack_into('<Q', m, 272, 512)
        m[512:520] = b'RSD PTR '
        m[527] = 2
        struct.pack_into('<IQ', m, 532, 36, 768)
        m[520] = -sum(m[512:532]) % 256
        m[544] = -sum(m[512:548]) % 256
        m[768:772] = b'XSDT'
        struct.pack_into('<I', m, 772, 44)
        struct.pack_into('<Q', m, 804, 1024)
        m[777] = -sum(m[768:812]) % 256
        m[1024:1028] = b'MCFG'
        struct.pack_into('<I', m, 1028, 60)
        m[1033] = -sum(m[1024:1084]) % 256
        self.header_crc()

    def header_crc(self):
        struct.pack_into('<I', self.memory, 16, 0)
        struct.pack_into('<I', self.memory, 16, zlib.crc32(self.memory[:120]))

    def read(self, address, size):
        if not 0 <= address <= len(self.memory)-size:
            raise ValueError('Out-of-bounds physical read')
        return bytes(self.memory[address:address+size])

    def test_published_configuration_path(self):
        result = observe(self.read, 0, 4096)
        self.assertEqual(result['rsdp'], 512)
        self.assertEqual(result['tables'][0]['signature'], 'MCFG')

    def test_signature_without_header_crc_is_not_evidence(self):
        self.memory[24] ^= 1
        with self.assertRaisesRegex(ValueError, 'found 0'):
            observe(self.read, 0, 4096)

    def test_bad_child_checksum_is_rejected(self):
        self.memory[1040] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            observe(self.read, 0, 4096)

    def test_configuration_count_is_bounded(self):
        struct.pack_into('<Q', self.memory, 104, 257)
        self.header_crc()
        with self.assertRaisesRegex(ValueError, 'found 0'):
            observe(self.read, 0, 4096)

    def test_unmapped_rsdp_rejected(self):
        struct.pack_into('<Q', self.memory, 272, 1 << 63)
        with self.assertRaisesRegex(ValueError, 'Out-of-bounds'):
            observe(self.read, 0, 4096)


if __name__ == '__main__':
    unittest.main()
