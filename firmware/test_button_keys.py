import pathlib,subprocess,tempfile,unittest
R=pathlib.Path(__file__).resolve().parent
SRC=r'''
#include <assert.h>
#include "button-keys.h"
int main(void){struct ps5_button_keys s;ps5_button_init(&s,1000);
 assert(ps5_button_event(&s,8,0x8011,5)==0);              /* up without down */
 /* r5.2: short press -> Down only after the 400 ms double-press window */
 ps5_button_event(&s,8,0x8010,100);assert(ps5_button_event(&s,8,0x8011,1100)==0);
 assert(ps5_button_tick(&s,1300)==0);assert(ps5_button_tick(&s,1501)==PS5_KEY_DOWN);
 ps5_button_event(&s,8,0x8010,2000);assert(ps5_button_event(&s,8,0x8011,5000)==PS5_KEY_ENTER);
 ps5_button_event(&s,8,0x8010,6000);assert(ps5_button_event(&s,8,0x8011,16000)==0); /* >=8 s ignored */
 assert(ps5_button_event(&s,0x10,0x8010,1)==0);           /* other service */
 assert(ps5_button_pop(&s)==PS5_KEY_DOWN && ps5_button_pop(&s)==PS5_KEY_ENTER && ps5_button_pop(&s)==0);
 /* double press within 400 ms = Esc, no Down */
 ps5_button_event(&s,8,0x8010,20000);ps5_button_event(&s,8,0x8011,20100);
 ps5_button_event(&s,8,0x8010,20300);assert(ps5_button_event(&s,8,0x8011,20400)==PS5_KEY_ESC);
 assert(ps5_button_tick(&s,30000)==0);assert(ps5_button_pop(&s)==PS5_KEY_ESC && ps5_button_pop(&s)==0);
 /* pending short then long press: Down first, then Enter */
 ps5_button_event(&s,8,0x8010,40000);ps5_button_event(&s,8,0x8011,40100);
 ps5_button_event(&s,8,0x8010,40200);assert(ps5_button_event(&s,8,0x8011,43200)==PS5_KEY_ENTER);
 assert(ps5_button_pop(&s)==PS5_KEY_DOWN && ps5_button_pop(&s)==PS5_KEY_ENTER);
 /* two short presses too far apart = two Downs */
 ps5_button_event(&s,8,0x8010,50000);ps5_button_event(&s,8,0x8011,50100);assert(ps5_button_tick(&s,50600)==PS5_KEY_DOWN);
 ps5_button_event(&s,8,0x8010,51000);ps5_button_event(&s,8,0x8011,51100);assert(ps5_button_tick(&s,51600)==PS5_KEY_DOWN);
 assert(ps5_button_pop(&s)==PS5_KEY_DOWN && ps5_button_pop(&s)==PS5_KEY_DOWN && ps5_button_pop(&s)==0);
 for(int i=0;i<20;i++){ps5_button_event(&s,8,0x8010,100000+i*1000);ps5_button_event(&s,8,0x8011,100000+i*1000+1);ps5_button_tick(&s,100000+i*1000+500);}
 assert(s.dropped==4 && ps5_button_pending(&s));
 return 0;}
'''
class ButtonKeyTests(unittest.TestCase):
    def test_state_machine(self):
        with tempfile.TemporaryDirectory() as w:
            p=pathlib.Path(w);(p/'t.c').write_text(SRC)
            subprocess.run(['cc','-std=c11','-Wall','-Werror','-I',str(R),str(p/'t.c'),str(R/'button-keys.c'),'-o',str(p/'t')],check=True)
            subprocess.run([str(p/'t')],check=True)
if __name__=='__main__':unittest.main()
