import os, subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from ps5_boot_reads import prepare_boot_reads,SOURCE

WORK = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work'))   # build tree (EDK2); set PSW_WORK for your own
EDK=(WORK / 'edk2')
class BootReadTests(unittest.TestCase):
    def test_actual_generated_wrapper_preserves_reads_and_bounds_snapshots(self):
        original=subprocess.check_output(['git','show','HEAD:'+SOURCE],cwd=EDK,text=True)
        with tempfile.TemporaryDirectory() as work:
            root=Path(work);(root/SOURCE).parent.mkdir(parents=True)
            with patch('ps5_boot_reads.subprocess.check_output',return_value=original):
                prepare_boot_reads(root)
                generated=(root/SOURCE).read_text()
                prepare_boot_reads(root)
                self.assertEqual(generated,(root/SOURCE).read_text())
            start=generated.index('  FAT_IFILE *IFile=IFILE_FROM_FHAND(FHand);')
            wrapper=generated[start:generated.index('\n}',start)]
            code=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <wchar.h>
typedef uint64_t UINT64;typedef size_t UINTN;typedef unsigned EFI_STATUS;
typedef unsigned EFI_TPL;typedef int BOOLEAN;typedef wchar_t CHAR16;
#define CONST const
#define TRUE 1
#define FALSE 0
#define EFI_ERROR(s) ((s)!=0)
#define TPL_HIGH_LEVEL 31
#define TPL_CALLBACK 8
#define StrCmp wcscmp
#define ReadData 0
#define DEBUG(x) ((void)0)
#define IFILE_FROM_FHAND(h) (h)
typedef struct {CHAR16 *FileString;} DIRENT;
typedef struct {DIRENT *DirEnt;UINTN FileSize;} FAT_OFILE;
typedef struct {FAT_OFILE *OFile;UINT64 Position;} FAT_IFILE;
static UINTN mPs5ReadSnapshots;static BOOLEAN mPs5ReadBusy;
static int mPs5ReadSnapshotGuid,mPs5WimEofGuid,WimEvents,Events,Calls,Mode,Locked;
static EFI_TPL Raise(EFI_TPL p){assert(p==31 && !Locked);return 4;}
static void Restore(EFI_TPL p){assert(p==4);}
static struct {EFI_TPL (*RaiseTPL)(EFI_TPL);void (*RestoreTPL)(EFI_TPL);} Bs={Raise,Restore},*gBS=&Bs;
static void EfiEventGroupSignal(void *g){assert(!Locked);if(g==&mPs5WimEofGuid){assert(Events==3);WimEvents++;}else Events++;}
static EFI_STATUS FatIFileAccess(FAT_IFILE *h,int rw,UINTN *n,void *buffer,void *token){
 Calls++;assert(!Locked);Locked=1;if(n && Mode!=1)h->Position+=*n;Locked=0;return Mode==1?7:0;
}
static EFI_STATUS run(FAT_IFILE *FHand,UINTN *BufferSize,void *Buffer){
WRAPPER
}
int main(void){
 DIRENT dir={L"boot.wim"};FAT_OFILE file={&dir,630590015};FAT_IFILE h={&file,0};
 UINTN size=4096;assert(run(&h,&size,0)==0 && h.Position==4096 && Events==1 && Calls==1 && WimEvents==0);
 size=4096;assert(run(&h,&size,0)==0 && Events==1);
 h.Position=256ULL*1024*1024-4096;assert(run(&h,&size,0)==0 && Events==2);
 h.Position=file.FileSize-4096;assert(run(&h,&size,0)==0 && Events==3 && WimEvents==1);
 dir.FileString=L"other.wim";h.Position=0;assert(run(&h,&size,0)==0 && Events==3 && WimEvents==1);
 dir.FileString=L"boot.sdi";Mode=1;assert(run(&h,&size,0)==7 && Events==4);
 for(int i=0;i<20;i++)assert(run(&h,&size,0)==7);
 assert(Events==8 && mPs5ReadSnapshots==8 && !mPs5ReadBusy);
}
'''.replace('WRAPPER',wrapper)
            source=root/'test.c';source.write_text(code);exe=root/'test'
            subprocess.run(['gcc','-std=c11',str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
