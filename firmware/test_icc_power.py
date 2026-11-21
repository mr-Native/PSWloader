import ctypes as C
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

class IccPowerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();out=Path(cls.work.name)/'icc.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',str(Path(__file__).with_name('icc-power.c')),'-o',str(out)],check=True)
        cls.lib=C.CDLL(str(out))
        cls.Read=C.CFUNCTYPE(C.c_uint32,C.c_void_p,C.c_uint,C.c_uint,C.c_uint)
        cls.Write=C.CFUNCTYPE(None,C.c_void_p,C.c_uint,C.c_uint,C.c_uint,C.c_uint32)
        cls.Ticks=C.CFUNCTYPE(C.c_uint64,C.c_void_p)
        cls.Relax=C.CFUNCTYPE(None,C.c_void_p)
        class IO(C.Structure):
            _fields_=[('context',C.c_void_p),('read',cls.Read),('write',cls.Write),('ticks',cls.Ticks),('relax',cls.Relax),('timeout',C.c_uint64)]
        cls.Notify=C.CFUNCTYPE(None,C.c_void_p,C.c_uint8,C.c_uint16,C.c_void_p,C.c_uint)
        cls.IO=IO;cls.lib.ps5_icc_power.argtypes=[C.POINTER(IO),C.c_int]
        cls.lib.ps5_icc_query.argtypes=[C.POINTER(IO),C.c_uint8,C.c_uint16,C.c_void_p,C.c_uint,C.c_void_p,C.c_uint,C.POINTER(C.c_uint)]
        cls.lib.ps5_icc_query_events.argtypes=cls.lib.ps5_icc_query.argtypes+[cls.Notify,C.c_void_p]
        cls.lib.ps5_icc_poll.argtypes=[C.POINTER(IO),cls.Notify,C.c_void_p]
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()

    def run_power(self,reboot=1,mode='ok',wrap=False,generic=None,events=False,poll=False):
        request=bytearray(0x7f0);replies=[];sent=[];writes=[];read_offsets=[];clock=[(1<<64)-10 if wrap else 0]
        def response(data,kind='ok'):
            rep=bytearray(data);struct.pack_into('<H',rep,2,struct.unpack_from('<H',rep,2)[0]|0x4000)
            if kind=='stale':struct.pack_into('<H',rep,6,0xeeee)
            if kind=='notification':struct.pack_into('<H',rep,2,0x8010)
            rep[10:12]=b'\0\0';struct.pack_into('<H',rep,10,sum(rep)&0xffff)
            if kind=='bad':rep[10]^=1
            if kind=='oversize':struct.pack_into('<H',rep,8,0xffff)
            return rep
        def read(_,region,offset,width):
            read_offsets.append((region,offset,width))
            if region==1:return 1 if replies else 0
            if offset==6:return 0x1234
            if offset==0x7f4:return 0 if mode=='busy' else 1
            if offset==0x7f0:return 0
            if 0x800<=offset<0xff0 and replies:
                start=offset-0x800
                return int.from_bytes(replies[0][start:start+width],'little')
            return 0
        def write(_,region,offset,width,value):
            writes.append((region,offset,width,value))
            if region==0 and offset<0x7f0:request[offset]=value
            if region==1 and offset==4:
                if value==1:
                    wire=bytes(request[:struct.unpack_from('<H',request,8)[0]])
                    sent.append(wire)
                    if mode=='timeout':return
                    if mode=='flood':
                        replies.append(response(wire,'notification'));return
                    if mode in ('notification','stale'):replies.append(response(wire,mode))
                    replies.append(response(wire,mode if mode in ('bad','oversize') else 'ok'))
                elif value==2 and replies:
                    old=replies.pop(0)
                    if mode=='flood':replies.append(old)
        def tick(_):clock[0]=(clock[0]+1)&((1<<64)-1);return clock[0]
        io=self.IO(None,self.Read(read),self.Write(write),self.Ticks(tick),self.Relax(lambda _:None),30)
        self.notifications=[]
        def notification(_,service,type,payload,length):
            self.notifications.append((service,type,C.string_at(payload,length),writes[-1]))
        callback=self.Notify(notification)
        if poll:
            if mode!='empty':
                wire=bytearray(32);wire[0]=0x42;wire[1]=0x10
                struct.pack_into('<H',wire,8,32)
                replies.append(response(wire,mode))
            code=self.lib.ps5_icc_poll(C.byref(io),callback,None)
            return code,sent,writes,read_offsets
        if generic is not None:
            payload,capacity,type=generic
            source=C.create_string_buffer(payload)
            output=(C.c_uint8*max(1,capacity))(*([0xa5]*max(1,capacity)))
            length=C.c_uint(999)
            args=(C.byref(io),2,type,source,len(payload),output,capacity,C.byref(length))
            code=(self.lib.ps5_icc_query_events(*args,callback,None) if events else self.lib.ps5_icc_query(*args))
            return code,sent,bytes(output),length.value,writes
        code=self.lib.ps5_icc_power(C.byref(io),reboot)
        return code,sent,writes,read_offsets

    def test_wire_messages_and_checksums(self):
        for reboot in (0,1):
            code,sent,_,_=self.run_power(reboot)
            self.assertEqual(code,0);self.assertEqual(len(sent),3)
            self.assertEqual([m[1] for m in sent],[2,2,4])
            self.assertEqual([struct.unpack_from('<H',m,6)[0] for m in sent],[0x1235,0x1236,0x1237])
            self.assertEqual(sent[0][12:21],bytes(9))
            self.assertEqual(sent[1][12:21],bytes([1])+bytes(8))
            self.assertEqual(sent[2][12:21],bytes([0,reboot,2,0,1,0,0,0,0]))
            for m in sent:
                self.assertEqual(struct.unpack_from('<H',m,10)[0],sum(m[:10]+m[12:])&0xffff)
                self.assertEqual(m[8:10],b'\x20\0')
    def test_idle_timeout_no_writes(self):
        code,sent,writes,_=self.run_power(mode='busy')
        self.assertEqual((code,sent,writes),(-2,[],[]))
    def test_reply_timeout_stops_sequence(self):
        code,sent,_,_=self.run_power(mode='timeout')
        self.assertEqual(code,-3);self.assertEqual(len(sent),1)
    def test_corrupt_or_oversize_reply(self):
        for mode in ('bad','oversize'):
            code,sent,_,reads=self.run_power(mode=mode)
            self.assertEqual(code,-4);self.assertEqual(len(sent),1)
            if mode=='oversize':self.assertFalse(any(r==0 and o>=0x800 and w==1 for r,o,w in reads))
    def test_notifications_and_stale_reply_do_not_complete_request(self):
        for mode in ('notification','stale'):
            code,sent,writes,_=self.run_power(mode=mode)
            self.assertEqual(code,0);self.assertEqual(len(sent),3)
            self.assertEqual(writes.count((1,4,4,2)),6)
    def test_tick_wrap(self):self.assertEqual(self.run_power(wrap=True)[0],0)
    def test_invalid_reset_kind_no_io(self):
        self.assertEqual(self.run_power(reboot=2)[:3],(-1,[],[]))

    def test_generic_payload_roundtrip_and_padding(self):
        for length in (0,1,20,21,255,0x7e4):
            payload=bytes(i%251 for i in range(length))
            code,sent,output,reply_bytes,_=self.run_power(generic=(payload,0x7e4,0x16))
            self.assertEqual(code,0)
            self.assertEqual(len(sent),1)
            self.assertEqual(len(sent[0]),max(32,12+length))
            self.assertEqual(reply_bytes,max(20,length))
            self.assertEqual(output[:length],payload)
            self.assertEqual(output[length:reply_bytes],bytes(reply_bytes-length))
            self.assertEqual(struct.unpack_from('<H',sent[0],10)[0],sum(sent[0][:10]+sent[0][12:])&0xffff)

    def test_generic_small_output_acks_without_partial_copy(self):
        code,sent,output,length,writes=self.run_power(generic=(bytes(24),23,0x16))
        self.assertEqual((code,output,length),(-5,bytes([0xa5])*23,24))
        self.assertIn((1,4,4,2),writes)

    def test_generic_invalid_length_or_reply_type_no_io(self):
        for data,type in ((bytes(0x7e5),0x16),(b'',0x4016),(b'',0x8016)):
            code,sent,_,length,writes=self.run_power(generic=(data,40,type))
            self.assertEqual((code,sent,length,writes),(-1,[],0,[]))

    def test_generic_corruption_does_not_expose_reply_data(self):
        code,_,output,length,_=self.run_power(mode='bad',generic=(b'hello',40,0x16))
        self.assertEqual((code,output,length),(-4,bytes([0xa5])*40,0))

    def test_query_delivers_notification_after_ack_and_still_waits_for_reply(self):
        result=self.run_power(mode='notification',generic=(b'event',40,0x16),events=True)
        self.assertEqual(result[0],0)
        self.assertEqual(len(self.notifications),1)
        service,type,payload,last_write=self.notifications[0]
        self.assertEqual((service,type,payload[:5],last_write),(2,0x8010,b'event',(1,4,4,2)))
        self.assertEqual(result[2][:5],b'event')

    def test_stale_replies_are_not_notifications(self):
        self.assertEqual(self.run_power(mode='stale',generic=(b'',40,0x16),events=True)[0],0)
        self.assertEqual(self.notifications,[])

    def test_notification_flood_does_not_extend_deadline(self):
        self.assertEqual(self.run_power(mode='flood',generic=(b'',40,0x16),events=True)[0],-3)
        self.assertGreater(len(self.notifications),0)
        self.assertLessEqual(len(self.notifications),30)

    def test_poll_is_bounded_and_never_sends_request(self):
        for mode,expected in (('empty',0),('notification',1),('stale',2),('bad',-4)):
            code,sent,writes,_=self.run_power(mode=mode,poll=True)
            self.assertEqual((code,sent),(expected,[]))
            self.assertNotIn((1,4,4,1),writes)
            self.assertEqual(len(self.notifications),int(mode=='notification'))

if __name__=='__main__':unittest.main()
