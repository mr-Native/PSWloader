"""edid6 host test: ps5-edid-info.c (firmware log + edidread.exe) against an independent Python EDID parser."""
import json, os, subprocess, tempfile, unittest
from pathlib import Path
WORK = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work'))   # build tree (EDK2, testdata); set PSW_WORK for your own
HERE = Path(__file__).parent.resolve()
EDIDS = [HERE / 'testdata' / 'dell-s3422dwg-edid.bin', HERE / 'testdata' / 'converter-fallback-MN864739-20261102.bin']
HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "ps5-edid-info.h"
int main(int argc,char**argv){
 unsigned char e[2048];FILE*f=fopen(argv[1],"rb");unsigned n=(unsigned)fread(e,1,sizeof e,f);fclose(f);
 if(argc>2)e[0]=1;                                   /* PS5 byte-0 quirk */
 struct ps5_edid_summary s;if(ps5_edid_summary(e,n,&s))return 2;
 printf("{\"vendor\":\"%s\",\"product\":%u,\"serial\":%u,\"name\":\"%s\",\"ext\":%u,\"header\":%u,\"cta\":%u,\"byte0\":%u,\"dtd\":[",
   s.vendor,s.product,s.serial,s.name,s.extensions,s.header_ok,s.cta,s.raw_byte0);
 for(unsigned i=0;i<s.dtd_count;i++)printf("%s[%u,%u,%u,%u,%u,%u]",i?",":"",s.dtd[i].pclk_khz,s.dtd[i].hact,s.dtd[i].vact,s.dtd[i].htot,s.dtd[i].vtot,s.dtd[i].refresh_mhz);
 printf("],\"vic\":[");for(unsigned i=0;i<s.vic_count;i++)printf("%s%u",i?",":"",s.vic[i]);
 unsigned char c[256];memcpy(c,e,256);unsigned char fs=ps5_edid_fix(c);unsigned sum=0;for(int i=0;i<128;i++)sum+=c[i];
 printf("],\"fixsum\":%u,\"fixok\":%u,\"crc\":%u}\n",fs,(sum&255)==0,ps5_edid_crc32(c,256));return 0;}
'''

def ref(e, quirk=False):
    e = bytearray(e)
    if quirk: e[0] = 1
    m = e[8] << 8 | e[9]
    r = {'vendor': ''.join(chr(64 + ((m >> s) & 31)) for s in (10, 5, 0)), 'product': e[10] | e[11] << 8,
         'serial': int.from_bytes(e[12:16], 'little'), 'name': '', 'ext': e[126],
         'header': int(e[1:7] == b'\xff' * 6 and e[7] == 0), 'byte0': e[0], 'dtd': [], 'vic': [], 'cta': 0}
    def dtd(d):
        p = d[0] | d[1] << 8
        if not p: return False
        ha = d[2] | (d[4] & 0xf0) << 4; hb = d[3] | (d[4] & 0x0f) << 8
        va = d[5] | (d[7] & 0xf0) << 4; vb = d[6] | (d[7] & 0x0f) << 8
        ht, vt = ha + hb, va + vb
        r['dtd'].append([p * 10, ha, va, ht, vt, (p * 10000 * 1000 + ht * vt // 2) // (ht * vt)])
        return True
    for k in range(4):
        d = e[54 + 18 * k:72 + 18 * k]
        if not dtd(d) and d[3] == 0xfc:
            r['name'] = bytes(d[5:18]).split(b'\n')[0].decode('ascii', 'replace').rstrip(' ')
    for b in range(1, min(len(e) // 128, e[126] + 1)):
        x = e[128 * b:128 * b + 128]
        if x[0] != 2: continue
        r['cta'] = 1
        end = x[2] if 4 <= x[2] <= 127 else 127
        p = 4
        while p < end:
            tag, ln = x[p] >> 5, x[p] & 31
            if p + 1 + ln > end: break
            if tag == 2:
                for v in x[p + 1:p + 1 + ln]:
                    r['vic'].append(v & 0x7f if 129 <= v <= 192 else v)
            p += 1 + ln
        p = x[2]
        while 4 <= p and p + 18 <= 127 and (x[p] | x[p + 1]):
            dtd(x[p:p + 18]); p += 18
    c = bytearray(e[:256]); c[0] = 0; c[127] = (-sum(c[:127])) & 0xff
    import zlib
    r.update(fixsum=c[127], fixok=1, crc=zlib.crc32(bytes(c)))
    r['dtd'] = r['dtd'][:8]; r['vic'] = r['vic'][:64]
    return r


class EdidInfoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(); w = Path(cls.work.name)
        (w / 'h.c').write_text(HARNESS)
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(HERE), str(w / 'h.c'),
                        str(HERE / 'ps5-edid-info.c'), '-o', str(w / 'h')], check=True)
        cls.exe = w / 'h'

    @classmethod
    def tearDownClass(cls): cls.work.cleanup()

    def test_against_reference(self):
        for path in EDIDS:
            for quirk in (False, True):
                out = json.loads(subprocess.run([str(self.exe), str(path)] + (['q'] if quirk else []),
                                                capture_output=True, text=True, check=True).stdout)
                self.assertEqual(out, ref(path.read_bytes(), quirk), path.name)

    def test_dell_values(self):
        out = json.loads(subprocess.run([str(self.exe), str(EDIDS[0]), 'q'], capture_output=True, text=True, check=True).stdout)
        self.assertEqual((out['vendor'], out['name'], out['dtd'][0][:3]), ('DEL', 'DELL S3422DWG', [319750, 3440, 1440]))
        self.assertIn(16, out['vic'])


if __name__ == '__main__':
    unittest.main()
