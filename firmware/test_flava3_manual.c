#include <stdio.h>
#include <string.h>
#include "flava3-manual.h"
static int f;
#define C(x) do{ if(!(x)){printf("FAIL %d %s\n",__LINE__,#x);f++;} }while(0)
int main(void){
  static const uint8_t c60[13]  = {0x0f,0x0e,0x1f,0x00,0x8f,0x5a,0x51,0x00,0x9f,0x8c,0x00,0xc9,0x05};   /* HW-proven stage 14 */
  static const uint8_t c100[13] = {0x0f,0x0e,0x1f,0x00,0x5f,0xf2,0x52,0x00,0x9f,0x8c,0x00,0xe6,0x05};
  struct flava3_manual m;
  C(flava3_manual_build(3440,3488,3520,3600,1440,1443,1453,1481,319750,1,0,&m)==0);
  C(memcmp(m.r7063,c60,11)==0 && m.r7042[2]==0x27 && m.r7042[3]==0 && m.r7042[4]==0x6f && m.r7042[5]==0x0d);
  C(flava3_manual_build(3440,3488,3520,3600,1440,1443,1453,1510,543500,1,0,&m)==0);
  C(memcmp(m.r7063,c100,11)==0 && m.r7042[2]==0x44 && m.r7078[0]==0x1f && m.r7078[1]==0x82);
  C(flava3_manual_build(1920,2008,2052,2200,1080,1084,1089,1125,148500,1,1,&m)==-1);   /* +H+V: not manual */
  C(flava3_manual_build(4096,4184,4272,4400,2160,2168,2178,2250,594000,1,0,&m)==-1);   /* wider than 3840 */
  C(flava3_manual_build(2560,2608,2640,2720,1440,1443,1448,1481,700000,1,0,&m)==-1);   /* > 600 MHz */
  printf("%s (%d)\n",f?"FAILED":"OK",f); return f; }
