/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * UTC calendar conversion, no hardware or service calls. */
#ifndef PS5_TIME_CALENDAR_H
#define PS5_TIME_CALENDAR_H
#include <Uefi.h>
STATIC BOOLEAN Ps5Leap(UINTN Y) { return Y%4==0 && (Y%100!=0 || Y%400==0); }
STATIC UINTN Ps5MonthDays(UINTN Y,UINTN M) {
  CONST UINT8 Days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  return Days[M-1]+(M==2 && Ps5Leap(Y));
}
STATIC BOOLEAN Ps5TimeToEpoch(CONST EFI_TIME *T,UINT64 *Seconds) {
  if(T==NULL || T->Year<1970 || T->Year>9999 || T->Month<1 || T->Month>12 ||
     T->Day<1 || T->Day>Ps5MonthDays(T->Year,T->Month) || T->Hour>23 ||
     T->Minute>59 || T->Second>59 || T->Nanosecond>=1000000000U ||
     (T->TimeZone!=EFI_UNSPECIFIED_TIMEZONE && (T->TimeZone < -1440 || T->TimeZone > 1440)) ||
     (T->Daylight & ~3U)!=0) return FALSE;
  UINT64 Days=0;
  for(UINTN Y=1970;Y<T->Year;Y++) Days+=365U+Ps5Leap(Y);
  for(UINTN M=1;M<T->Month;M++) Days+=Ps5MonthDays(T->Year,M);
  INT64 Value=(INT64)((Days+T->Day-1)*86400+T->Hour*3600+T->Minute*60+T->Second);
  if(T->TimeZone!=EFI_UNSPECIFIED_TIMEZONE) Value-=(INT64)T->TimeZone*60;
  if(Value<0) return FALSE;
  *Seconds=(UINT64)Value;return TRUE;
}
STATIC BOOLEAN Ps5EpochToTime(UINT64 Seconds,UINT32 Nanos,EFI_TIME *T) {
  if(Seconds>253402300799ULL || Nanos>=1000000000U) return FALSE;
  UINT64 Days=Seconds/86400;UINTN Y=1970,M=1;
  while(Days>=365U+Ps5Leap(Y)) { Days-=365U+Ps5Leap(Y);Y++; }
  while(Days>=Ps5MonthDays(Y,M)) { Days-=Ps5MonthDays(Y,M);M++; }
  T->Year=(UINT16)Y;T->Month=(UINT8)M;T->Day=(UINT8)(Days+1);
  T->Hour=(UINT8)((Seconds%86400)/3600);T->Minute=(UINT8)((Seconds%3600)/60);
  T->Second=(UINT8)(Seconds%60);T->Nanosecond=Nanos;
  T->TimeZone=0;T->Daylight=0;T->Pad1=0;T->Pad2=0;
  return TRUE;
}
#endif
