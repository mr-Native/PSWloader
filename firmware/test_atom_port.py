import os,pathlib,subprocess,tempfile,struct,unittest
class AtomPortTests(unittest.TestCase):
 def test_host_core_returns_eot_and_rejects_invalid_instructions(self):
  r=pathlib.Path(__file__).parent/'atom-host'
  with tempfile.TemporaryDirectory() as work:
   p=pathlib.Path(work);subprocess.run(['gcc','-O1','-fsanitize=address,undefined',str(r/'atom-host.c'),'-o',str(p/'atom')],check=True)
   d=bytearray(65536);d[:2]=b'\x55\xaa';struct.pack_into('<H',d,0x48,0x100);d[0x104:0x108]=b'ATOM';struct.pack_into('<HH',d,0x11e,0x200,0x300)
   struct.pack_into('<HBBH',d,0x200,6,2,1,0x400);struct.pack_into('<HBBH',d,0x400,7,1,1,0);d[0x406]=91
   env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}
   rom=p/'rom.bin';rom.write_bytes(d)
   result=subprocess.run([str(p/'atom'),str(rom),'0'],capture_output=True,text=True,env=env);self.assertEqual(result.returncode,0,result.stderr);self.assertIn('writes=0',result.stdout)
   for opcode in (0,127,255):
    d[0x406]=opcode;rom.write_bytes(d);result=subprocess.run([str(p/'atom'),str(rom),'0'],capture_output=True,text=True,env=env)
    self.assertNotEqual(result.returncode,0);self.assertIn('invalid opcode',result.stderr)
   d[0x406]=91;struct.pack_into('<H',d,0x204,65535);rom.write_bytes(d)
   result=subprocess.run([str(p/'atom'),str(rom),'0'],capture_output=True,text=True,env=env);self.assertNotEqual(result.returncode,0)
