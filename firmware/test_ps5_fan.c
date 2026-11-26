/* host test ps5-fan.h v1.0 */
#include <stdio.h>
#include "ps5-fan.h"
int main(void)
{
    int fail = 0; unsigned char rep[32] = {0}, set[PS5_FAN_SET_BYTES], get[PS5_FAN_GET_BYTES]; unsigned w[6], d;
    unsigned words[6] = { 70u << 8, 5243, 0, 0, 64, 16 };
    for (int i = 0; i < 6; i++) { rep[4+4*i] = (unsigned char)words[i]; rep[5+4*i] = (unsigned char)(words[i] >> 8); }
    if (ps5_fan_decode(rep, 28, w) || w[0] >> 8 != 70 || w[1] != 5243 || w[5] != 16) { puts("FAIL decode"); fail++; }
    if (!ps5_fan_decode(rep, 27, w)) { puts("FAIL short"); fail++; }
    ps5_fan_decode(rep, 28, w); ps5_fan_set_request(set, w, 60);
    if (set[0] || set[1] || set[4] || set[5] != 60 || set[8] != (5243 & 255) || set[9] != (5243 >> 8) || set[24] != 16) { puts("FAIL set"); fail++; }
    ps5_fan_get_request(get); if (get[0] || get[19]) { puts("FAIL get"); fail++; }
    unsigned char du[4] = { 0, 0, 0x00, 0x02 }; if (ps5_fan_duty(du, 4, &d) || d != 512) { puts("FAIL duty"); fail++; }
    unsigned char bad[4] = { 0, 0, 0x01, 0x04 }; if (!ps5_fan_duty(bad, 4, &d)) { puts("FAIL duty range"); fail++; }
    if (ps5_fan_target_for_preset(0) != 72 || ps5_fan_target_for_preset(1) != 70 || ps5_fan_target_for_preset(2) != 66 || ps5_fan_target_for_preset(9) != 70)   /* r5.23: TM v1.6 presets */ { puts("FAIL preset"); fail++; }
    puts(fail ? "FAILED" : "ALL OK"); return fail;
}
