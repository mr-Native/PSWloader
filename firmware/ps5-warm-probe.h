/* ps5-warm-probe.h - lab only (HW 19.11.2026): after a warm re-entry from Windows the restarted firmware stays dark.
 * Ps5WarmProbeOff() switches the console off through the ICC if the trampoline's hand-over record (0x1_1180_6040,
 * "PSWWARM1") is present - a cold boot never has it - so a build with a probe point shows by its power draw whether the
 * re-entry got that far. Same three requests as ps5_icc_power (icc-power.c), reboot = 0, fixed physical mailbox
 * 0x85400000 / doorbell 0x85308000 (Salina 104D:9107 BAR4 / BAR2 + 0x108000). No globals, no function pointers. */
#ifndef PS5_WARM_PROBE_H
#define PS5_WARM_PROBE_H
static __attribute__((unused)) void Ps5WarmProbeOff (void) {
  if (*(volatile unsigned long long *)0x111806040ULL != 0x314D524157575350ULL) return;
  __asm__ __volatile__ ("cli");   /* no timer tick -> no ButtonPoll on the same mailbox in the middle of this */
  volatile unsigned char *M = (volatile unsigned char *)0x85400000ULL, *B = (volatile unsigned char *)0x85308000ULL;
  unsigned short Id = (unsigned short)(*(volatile unsigned short *)(M + 6) + 1);
  { /* (HW 20.11. lab93-101: no warm probe switched off since the soft timer lets ButtonPoll run first) - like
     * ps5pwr icc_prepare / Ps5IccWarmPrepare: drain up to 16 inbound messages, then, if the mailbox is not idle within
     * ~100 ms nominal, put the transport back to idle (SOW 0, SOR 1, latched status cleared) before the first request */
    for (unsigned I = 0; I < 16; I++) {
      unsigned St = *(volatile unsigned *)(B + 0x14) & 3;
      if (St) *(volatile unsigned *)(B + 0x14) = St;
      if (!(St & 1)) break;
      *(volatile unsigned short *)(M + 0xFF0) = 0; *(volatile unsigned short *)(M + 0xFF4) = 1; *(volatile unsigned *)(B + 4) = 2;
    }
    unsigned Lo, Hi; __asm__ __volatile__ ("rdtsc" : "=a" (Lo), "=d" (Hi));
    unsigned long long T0 = ((unsigned long long)Hi << 32) | Lo, Now = T0;
    while (!(*(volatile unsigned short *)(M + 0x7F4) == 1 && *(volatile unsigned short *)(M + 0x7F0) == 0) && Now - T0 < 1596300000ULL / 10) {
      __asm__ __volatile__ ("pause; rdtsc" : "=a" (Lo), "=d" (Hi)); Now = ((unsigned long long)Hi << 32) | Lo;
    }
    if (!(*(volatile unsigned short *)(M + 0x7F4) == 1 && *(volatile unsigned short *)(M + 0x7F0) == 0)) {
      *(volatile unsigned *)(B + 0x14) = 3;
      *(volatile unsigned short *)(M + 0x7F0) = 0;
      *(volatile unsigned short *)(M + 0x7F4) = 1;
    }
  }
  for (unsigned Q = 0; Q < 3; Q++) {
    unsigned char R[32]; unsigned short Sum = 0; unsigned char Svc = Q < 2 ? 2 : 4; unsigned short Type = Q < 2 ? 0x10 : 1;
    for (unsigned I = 0; I < 32; I++) R[I] = 0;
    R[0] = 0x42; R[1] = Svc; R[2] = (unsigned char)Type; R[3] = (unsigned char)(Type >> 8); R[4] = 3;
    R[6] = (unsigned char)Id; R[7] = (unsigned char)(Id >> 8); R[8] = 32;
    if (Q == 1) R[12] = 1;
    if (Q == 2) { R[14] = 2; R[16] = 1; }
    for (unsigned I = 0; I < 32; I++) if (I != 10 && I != 11) Sum = (unsigned short)(Sum + R[I]);
    R[10] = (unsigned char)Sum; R[11] = (unsigned char)(Sum >> 8);
    for (unsigned long N = 0; N < 200000000UL && !(*(volatile unsigned short *)(M + 0x7F4) == 1 && *(volatile unsigned short *)(M + 0x7F0) == 0); N++) {
      unsigned St = *(volatile unsigned *)(B + 0x14) & 3;
      if (St) *(volatile unsigned *)(B + 0x14) = St;
      if (St & 1) { *(volatile unsigned short *)(M + 0xFF0) = 0; *(volatile unsigned short *)(M + 0xFF4) = 1; *(volatile unsigned *)(B + 4) = 2; }
      __asm__ __volatile__ ("pause");
    }
    *(volatile unsigned *)(B + 0x14) = 2;
    *(volatile unsigned short *)(M + 0x7F4) = 0;
    for (unsigned I = 0; I < 32; I++) M[I] = R[I];
    *(volatile unsigned short *)(M + 0x7F0) = 1;
    *(volatile unsigned *)(B + 4) = 1;
    for (unsigned long N = 0; N < 200000000UL; N++) {
      unsigned St = *(volatile unsigned *)(B + 0x14) & 3;
      if (St) *(volatile unsigned *)(B + 0x14) = St;
      if (St & 1) {
        int Mine = M[0x801] == Svc && *(volatile unsigned short *)(M + 0x806) == Id;
        *(volatile unsigned short *)(M + 0xFF0) = 0; *(volatile unsigned short *)(M + 0xFF4) = 1; *(volatile unsigned *)(B + 4) = 2;
        if (Mine) break;
      }
      __asm__ __volatile__ ("pause");
    }
    Id++;
  }
  for (;;) __asm__ __volatile__ ("cli; hlt");
}
/* --warm-probe 40: an unhandled DXE exception after a warm re-entry waits (vector + 1) * 10 s, then switches off - the
 * time from the restart to 0 W gives the vector (14 = #PF -> 150 s); TSC 1,596,300,000 Hz as Ps5TimerLib */
static __attribute__((unused)) void Ps5WarmProbeVectorOff (unsigned Vector) {
  if (*(volatile unsigned long long *)0x111806040ULL != 0x314D524157575350ULL) return;
  unsigned long long Lo, Hi, Start, Now, Wait = (unsigned long long)(Vector + 1) * 10ULL * 1596300000ULL;
  __asm__ __volatile__ ("rdtsc" : "=a" (Lo), "=d" (Hi)); Start = Lo | (Hi << 32);
  do { __asm__ __volatile__ ("pause; rdtsc" : "=a" (Lo), "=d" (Hi)); Now = Lo | (Hi << 32); } while (Now - Start < Wait);
  Ps5WarmProbeOff ();
}
#define PS5_WARM_PROGRESS 0x111806100ULL   /* [0] last point, [1] write count, [2] armed magic (set only by the watchdog start) */
#define PS5_WARM_ARMED    0x4D52414757445750ULL   /* "PWDWGARM" */
static __attribute__((unused)) void Ps5WarmArm (void) {
  volatile unsigned long long *P = (volatile unsigned long long *)PS5_WARM_PROGRESS;
  P[0] = 40; P[1] = 0; P[2] = PS5_WARM_ARMED;
}
static __attribute__((unused)) void Ps5WarmProgress (unsigned N) {   /* writes nothing unless a warm watchdog armed the cell */
  volatile unsigned long long *P = (volatile unsigned long long *)PS5_WARM_PROGRESS;
  if (P[2] != PS5_WARM_ARMED) return;
  P[0] = N; P[1] = P[1] + 1;
#if !defined (PS5_WARM_EBS_WATCH)   /* lab --warm-ebs-watch: stays armed through ExitBootServices (MpInitLib parks the AP) */
  if (N >= 50) P[2] = 0;                                       /* disarmed before the OS: a later cold boot writes nothing */
#endif
}
#define PS5_WARM_TRAMP_PUB 0x111806180ULL   /* [0] "PSWTRAMP", [1] trampoline physical entry, [2] parameters physical */
#define PS5_WARM_RESCUE_AREA 0x111840000ULL   /* [0] "PSWRESCU", [1] last progress point, ring copy at +16 */
#define PS5_WARM_RESCUE_MAGIC 0x5543534557535350ULL   /* "PSWRESCU" */
#define PS5_WARM_TRAMP_MAGIC  0x504D415254575350ULL   /* "PSWTRAMP" */
/* RUN 2: the trampoline put flags bit 2 into the warm record (0x1_1180_6040: magic, flags) */
static __attribute__((unused)) int Ps5WarmRescueRun (void) {
  volatile unsigned long long *R = (volatile unsigned long long *)0x111806040ULL;
  return R[0] == 0x314D524157575350ULL && (R[1] & 4) != 0;
}
/* RUN 1, on the watchdog AP: copy the ring, set the rescue flag, enter the trampoline; never returns */
static __attribute__((unused)) void Ps5WarmRescueJump (unsigned long long Point) {
  volatile unsigned long long *Pub = (volatile unsigned long long *)PS5_WARM_TRAMP_PUB;
  volatile unsigned long long *Hdr = (volatile unsigned long long *)PS5_WARM_RESCUE_AREA;
  volatile unsigned char *Src = (volatile unsigned char *)0x111001000ULL, *Dst = (volatile unsigned char *)(PS5_WARM_RESCUE_AREA + 16);
  __asm__ __volatile__ ("cli");
  for (unsigned long I = 0; I < 65552UL; I++) Dst[I] = Src[I];
  Hdr[1] = Point;
  Hdr[0] = PS5_WARM_RESCUE_MAGIC;
  if (Pub[0] != PS5_WARM_TRAMP_MAGIC || Pub[1] == 0 || Pub[2] == 0) {
    *(volatile unsigned long long *)0x111806040ULL = 0x314D524157575350ULL;
    Ps5WarmProbeOff ();                                         /* no trampoline published: old ICC-off behaviour */
  }
  *(volatile unsigned long long *)(Pub[2] + 168) = 4;           /* PS5_WARM_PARAMS.NextFlags: bit 2 = rescue run */
  __asm__ __volatile__ ("mfence; wbinvd" ::: "memory");          /* the copy must be in DRAM before the INITs */
  ((void (*)(void))(Pub[1])) ();                                /* calling core becomes the boot core, INIT to the rest */
  for (;;) __asm__ __volatile__ ("cli; hlt");
}
/* lab --warm-ebs-steps: 48x48 squares in the top row after a warm run's ExitBootServices call, painted into the frame buffer
 * the BeforeExitBootServices notify published at 0x1_1180_6300 ("PSWSQFB1", base, stride) - no protocol, no globals */
#define PS5_WARM_SQ_CELL  0x111806300ULL
#define PS5_WARM_SQ_MAGIC 0x3142465153575350ULL   /* "PSWSQFB1" */
static __attribute__((unused)) void Ps5WarmSq (unsigned Slot, unsigned Color) {
  volatile unsigned long long *C = (volatile unsigned long long *)PS5_WARM_SQ_CELL;
  if (C[0] != PS5_WARM_SQ_MAGIC || C[1] == 0) return;
  volatile unsigned *Fb = (volatile unsigned *)C[1]; unsigned long long S = C[2];
  for (unsigned Y = 0; Y < 48; Y++) for (unsigned X = 0; X < 48; X++) Fb[Y * S + 320 + Slot * 60 + X] = Color;
  __asm__ __volatile__ ("wbinvd" ::: "memory");   /* the frame buffer is WB-cached: without a flush a stall hides the square */
}
/* --warm-probe 71: a signal without any PCIe/ICC access - 12 slots of 16 s (TSC nominal; ~8 s real at the observed 2x),
 * alternating heavy integer load and pause on this core, then halt. Visible as a square wave on the power meter if the
 * core still runs (the ICC/PCIe path may be blocked). */
static __attribute__((unused)) void Ps5WarmLoadBlink (void) {
  const unsigned long long Hz = 1596300000ULL;
  volatile unsigned long long X = 0x9E3779B97F4A7C15ULL;
  __asm__ __volatile__ ("cli");
  for (unsigned K = 0; K < 12; K++) {
    unsigned long long Lo, Hi, T0, T;
    __asm__ __volatile__ ("rdtsc" : "=a" (Lo), "=d" (Hi)); T0 = Lo | (Hi << 32);
    do {
      if ((K & 1) == 0) { for (unsigned I = 0; I < 4096; I++) X = X * 0x5851F42D4C957F2DULL + 1442695040888963407ULL; }
      else __asm__ __volatile__ ("pause");
      __asm__ __volatile__ ("rdtsc" : "=a" (Lo), "=d" (Hi)); T = Lo | (Hi << 32);
    } while (T - T0 < 16ULL * Hz);
  }
  for (;;) __asm__ __volatile__ ("cli; hlt");
}
/* the AP side: never returns before point 50; no boot services, no DEBUG, no locks */
static __attribute__((unused)) void Ps5WarmWatchdog (void) {
  volatile unsigned long long *P = (volatile unsigned long long *)PS5_WARM_PROGRESS;
#if defined (PS5_WARM_RESCUE)
  const unsigned long long Hz = 1596300000ULL, Idle = 40ULL * Hz;   /* rescue: 40 s without a new point */
#else
  const unsigned long long Hz = 1596300000ULL, Idle = 60ULL * Hz;
#endif
  unsigned long long Lo, Hi, Seen = P[1], Since, Now;
  __asm__ __volatile__ ("rdtsc" : "=a" (Lo), "=d" (Hi)); Since = Lo | (Hi << 32);
  for (;;) {
    __asm__ __volatile__ ("pause; rdtsc" : "=a" (Lo), "=d" (Hi)); Now = Lo | (Hi << 32);
#if defined (PS5_WARM_EBS_WATCH)
    if (P[2] != PS5_WARM_ARMED) return;
#else
    if (P[0] >= 50 || P[2] != PS5_WARM_ARMED) return;           /* Ps5LaunchWindows reached: done */
#endif
    if (P[1] != Seen) { Seen = P[1]; Since = Now; continue; }
    if (Now - Since < Idle) continue;
    break;
  }
#if defined (PS5_WARM_RESCUE)
  Ps5WarmRescueJump (P[0]);                                     /* never returns */
#endif
#if defined (PS5_WARM_EBS_WATCH)   /* 50 Ps5LaunchWindows, 51 BeforeExitBootServices, 52/53 EBS mark start/end, 54/55 IER notify
                                     * start/end: 60 s idle + (N - 49) * 30 s nominal; below 50 as before */
  { unsigned long long N = P[0] >= 50 ? (P[0] - 49) * 3 : (P[0] > 39 ? P[0] - 39 : 1), Start = Now;
#else
  { unsigned long long N = P[0] > 39 ? P[0] - 39 : 1, Start = Now;
#endif
    do { __asm__ __volatile__ ("pause; rdtsc" : "=a" (Lo), "=d" (Hi)); Now = Lo | (Hi << 32); } while (Now - Start < N * 10ULL * Hz); }
  { volatile unsigned char *M = (volatile unsigned char *)0x85400000ULL, *B = (volatile unsigned char *)0x85308000ULL;
    *(volatile unsigned *)(B + 0x14) = 3;                       /* ICC transport to idle (ps5pwr icc_prepare) - the BSP */
    *(volatile unsigned short *)(M + 0x7F0) = 0;                 /* may have hung inside a transaction */
    *(volatile unsigned short *)(M + 0x7F4) = 1; }
  *(volatile unsigned long long *)0x111806040ULL = 0x314D524157575350ULL;   /* Ps5WarmReadRecord may have cleared it */
  Ps5WarmProbeOff ();
}
#endif
