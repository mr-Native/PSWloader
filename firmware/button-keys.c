/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "button-keys.h"
void ps5_button_init(struct ps5_button_keys *s,uint64_t hz){
 if(!s)return;
 *s=(struct ps5_button_keys){0};
 s->hz=hz?hz:1;
}
static int push(struct ps5_button_keys *s,uint8_t k){
 if(s->head-s->tail>=sizeof(s->queue)){s->dropped++;return 0;}
 s->queue[s->head++%sizeof(s->queue)]=k;return k;}
#define PS5_DOUBLE_WINDOW(s) ((s)->hz*2/5)   /* 400 ms */
int ps5_button_tick(struct ps5_button_keys *s,uint64_t now){
 if(!s || !s->short_pending || s->pressed)return 0;
 if(now>=s->short_up_at && now-s->short_up_at>PS5_DOUBLE_WINDOW(s)){s->short_pending=0;return push(s,PS5_KEY_DOWN);}
 return 0;
}
int ps5_button_event(struct ps5_button_keys *s,uint8_t service,uint16_t type,uint64_t now){
 if(!s || service!=PS5_BUTTON_SERVICE)return 0;
 if(type==0x8010){s->pressed=1;s->down_at=now;return 0;}
 if(type!=0x8011 || !s->pressed)return 0;
 s->pressed=0;
 uint64_t held=now>=s->down_at?now-s->down_at:0;
 if(held<2*s->hz) {                                       /* short */
  if(s->short_pending && s->down_at>=s->short_up_at && s->down_at-s->short_up_at<=PS5_DOUBLE_WINDOW(s)) {
   s->short_pending=0;return push(s,PS5_KEY_ESC);         /* r5.2: double press = Esc */
  }
  if(s->short_pending){s->short_pending=0;push(s,PS5_KEY_DOWN);}   /* an older single press, window long over */
  s->short_pending=1;s->short_up_at=now;return 0;        /* Down follows from ps5_button_tick */
 }
 if(s->short_pending){s->short_pending=0;push(s,PS5_KEY_DOWN);}    /* keep order: pending Down before Enter */
 if(held<8*s->hz)return push(s,PS5_KEY_ENTER);
 return 0; /* >=8 s: treated as a hardware power gesture, never a key */
}
int ps5_button_pop(struct ps5_button_keys *s){
 if(!s || s->head==s->tail)return 0;
 return s->queue[s->tail++%sizeof(s->queue)];
}
int ps5_button_pending(const struct ps5_button_keys *s){return s && s->head!=s->tail;}
