"""Check the PS5 early handoff fix against pinned EDK source, without booting."""
import ctypes as C
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from ps5_entry_fixups import (X64_HANDOFF,PIC_WRITES,without_legacy_pic,prepare_x64_handoff,
    X64_PAGE_TABLES,CACHE_LEAVES,preserve_mmio_cache,prepare_x64_page_tables,
    CPU_SOURCE,quiesce_inherited_apic,prepare_cpu_apic)


@unittest.skipUnless((Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2')/CPU_SOURCE).exists(),
                     'Pinned CPU source required')
class EarlyApicTests(unittest.TestCase):
    def test_sources_masked_before_notification_can_enable_interrupts(self):
        root=Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2')
        original=subprocess.check_output(['git','show','HEAD:'+CPU_SOURCE],cwd=root,text=True)
        result=quiesce_inherited_apic(original)
        block=('  // Quiesce inherited BSP sources before CPU protocol notify can enable IF.\n'
               '  DisableApicTimerInterrupt ();\n  DisableLvtInterrupts ();\n\n')
        self.assertEqual(result.replace(block,''),original)
        self.assertLess(result.index('  InitInterruptDescriptorTable ();'),result.index(block))
        self.assertLess(result.index(block),result.index('  Status = gBS->InstallMultipleProtocolInterfaces ('))
        self.assertEqual(result.count('  DisableApicTimerInterrupt ();'),1)
        self.assertEqual(result.count('  DisableLvtInterrupts ();'),1)
        # Reapplying must fail closed, rather than accumulate hardware writes.
        with self.assertRaisesRegex(RuntimeError,'anchors'):quiesce_inherited_apic(result)
        with self.assertRaisesRegex(RuntimeError,'anchors'):
            quiesce_inherited_apic(original.replace('  // Make sure interrupts are disabled',''))
        with tempfile.TemporaryDirectory() as work:
            target=Path(work)/CPU_SOURCE;target.parent.mkdir(parents=True)
            target.write_text(original);prepare_cpu_apic(Path(work))
            self.assertEqual(target.read_text(),result)

    def test_changed_initialization_order_is_rejected(self):
        root=Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2')
        original=subprocess.check_output(['git','show','HEAD:'+CPU_SOURCE],cwd=root,text=True)
        bad=original.replace('  InitInterruptDescriptorTable ();','TEMP').replace(
            '  InitializeMpSupport ();','  InitInterruptDescriptorTable ();').replace('TEMP','  InitializeMpSupport ();')
        with self.assertRaisesRegex(RuntimeError,'order'):quiesce_inherited_apic(bad)

EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

@unittest.skipUnless((EDK/X64_HANDOFF).exists(),'EDK sources required')
class EntryFixupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=subprocess.check_output(['git','show','HEAD:'+X64_HANDOFF],cwd=EDK,text=True)
    def test_only_absent_pic_writes_removed(self):
        result=without_legacy_pic(self.source)
        self.assertNotIn('IoWrite8 (LEGACY_8259_',result)
        # Restore only the permitted edits: all FP/CR3/page-table/stack/HOB
        # operations, declarations and their order must remain byte-identical.
        restored=result.replace('  // PS5 has no legacy 8259 mask ports; preserve loader interrupt state.',
                                '  // Mask off all legacy 8259 interrupt sources')
        expected=self.source
        for line in PIC_WRITES:expected=expected.replace(line,'')
        self.assertEqual(restored,expected)
        for retained in ('InitializeFloatingPointUnits ();','AsmWriteCr3 (PageTables);',
                         'CreateIdentityMappingPageTables (','UpdateStackHob (','SwitchStack ('):
            self.assertEqual(result.count(retained),self.source.count(retained))
    def test_changed_or_repeated_anchors_fail_closed(self):
        for line in PIC_WRITES:
            for bad in (self.source.replace(line,''),self.source+line):
                with self.assertRaisesRegex(RuntimeError,'anchors'):without_legacy_pic(bad)
        bad=self.source.replace(PIC_WRITES[0],'TEMP').replace(PIC_WRITES[1],PIC_WRITES[0]).replace('TEMP',PIC_WRITES[1])
        with self.assertRaisesRegex(RuntimeError,'order'):without_legacy_pic(bad)
    def test_every_generation_uses_pristine_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);target=root/X64_HANDOFF;target.parent.mkdir(parents=True)
            with patch('ps5_entry_fixups.subprocess.check_output',return_value=self.source) as git:
                prepare_x64_handoff(root);first=target.read_text()
                target.write_text('stale generated content')
                prepare_x64_handoff(root)
                self.assertEqual(target.read_text(),first)
                self.assertEqual(git.call_count,2)
                self.assertEqual(git.call_args.args[0],['git','show','HEAD:'+X64_HANDOFF])

@unittest.skipUnless((EDK/X64_PAGE_TABLES).exists(),'EDK sources required')
class PageTableCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=subprocess.check_output(['git','show','HEAD:'+X64_PAGE_TABLES],cwd=EDK,text=True)
        cls.fixed=preserve_mmio_cache(cls.source)
        cls.work=tempfile.TemporaryDirectory();work=Path(cls.work.name)
        header=EDK/'UefiPayloadPkg/UefiPayloadEntry/X64/VirtualMemory.h'
        # Compile the actual four generated leaf initializers against EDK's
        # real 4KiB/2MiB/1GiB entry types; exercise without installing CR3.
        code=['#include <PiPei.h>','#include <Library/BaseLib.h>',
              '#include "'+str(header)+'"']
        for index,((anchor,address),typename,pointer) in enumerate(zip(CACHE_LEAVES,
                ('PAGE_TABLE_4K_ENTRY','PAGE_TABLE_ENTRY','PAGE_TABLE_1G_ENTRY','PAGE_TABLE_ENTRY'),
                ('PageTableEntry','PageDirectoryEntry','PageDirectory1GEntry','PageDirectoryEntry'))):
            for label,source in (('Original',cls.source),('Fixed',cls.fixed)):
                # Match by the unique physical-address operand, distinguishing
                # the two PageDirectoryEntry paths in the source.
                pattern=re.escape(anchor[:-1])+r'[^;]*;'
                statement=re.search(pattern,source).group(0)
                code.append('UINT64 '+label+str(index)+'(UINT64 Address,UINT64 AddressEncMask) {'+
                    typename+' Entry; '+typename+' *'+pointer+'=&Entry; UINT64 '+address+'=Address; '+
                    '(void)AddressEncMask; '+statement+
                    pointer+'->Bits.ReadWrite=1; '+pointer+'->Bits.Present=1; '+
                    (pointer+'->Bits.MustBe1=1; ' if index else '')+'return Entry.Uint64; }')
        (work/'leaves.c').write_text('\n'.join(code))
        subprocess.run(['gcc','-std=c11','-shared','-fPIC','-Wall','-Wextra','-Werror','-fshort-wchar',
            '-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),
            str(work/'leaves.c'),'-o',str(work/'leaves.so')],check=True)
        cls.lib=C.CDLL(str(work/'leaves.so'))
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def test_all_generated_leaf_sizes_preserve_mmio_uc_selection(self):
        for index,size in enumerate((0x1000,0x200000,0x40000000,0x200000)):
            original=getattr(self.lib,'Original'+str(index));fixed=getattr(self.lib,'Fixed'+str(index))
            for function in (original,fixed):
                function.argtypes=[C.c_uint64,C.c_uint64];function.restype=C.c_uint64
            for address in (0,0x80000000-size,0x80000000,0xc0000000,0x100000000-size,
                            0x100000000,0x980000000):
                for encryption in (0,1<<47):
                    with self.subTest(path=index,address=hex(address),encryption=encryption):
                        before=original(address,encryption);after=fixed(address,encryption)
                        self.assertEqual(after,before | (0x18 if 0x80000000<=address<0x100000000 else 0))
                        self.assertEqual((after>>3)&3,3 if 0x80000000<=address<0x100000000 else 0)
    def test_only_four_leaf_initializers_change_and_source_drift_rejected(self):
        restored=self.fixed
        for anchor,address in CACHE_LEAVES:
            pattern=re.escape(anchor[:-1])+r'[^;]*;'
            statement=re.search(pattern,restored).group(0)
            restored=restored.replace(statement,anchor)
            with self.assertRaisesRegex(RuntimeError,'leaf anchors'):
                preserve_mmio_cache(self.source.replace(anchor,''))
            with self.assertRaisesRegex(RuntimeError,'leaf anchors'):
                preserve_mmio_cache(self.source+anchor)
        self.assertEqual(restored,self.source)
    def test_page_source_regenerated_from_pinned_head(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);target=root/X64_PAGE_TABLES;target.parent.mkdir(parents=True)
            with patch('ps5_entry_fixups.subprocess.check_output',return_value=self.source) as git:
                prepare_x64_page_tables(root)
                target.write_text('stale')
                prepare_x64_page_tables(root)
                self.assertEqual(target.read_text(),self.fixed)
                self.assertEqual(git.call_args.args[0],['git','show','HEAD:'+X64_PAGE_TABLES])

if __name__=='__main__':unittest.main()
