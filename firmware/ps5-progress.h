/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-progress.h v1.0 (25.11.2026, r5.27.2, GitHub issue #14 "offline probe") - firmware progress cell for consoles we have no
 * lab for (6.00 fat: powered off ~15 s after the wake, no firmware log on the stick).
 *
 * The firmware writes each milestone into a cell of the loader's reserved arena (0x100000000..0x111000000 ends at
 * 0x111000000; the bootstrap breadcrumb PSWBOOT1 is at 0x111000000, the PCI lock at 0x111000100, the warm test marker
 * at 0x111000300 - this cell is 0x111000400) and pushes it to DRAM (WBINVD), because a power cut drops the caches.
 * PSWloader.elf reads both cells at its next start and writes them into pswloader-elf.log, then clears them. DRAM is not
 * guaranteed to survive the power-off, so a missing cell proves nothing; a present one names the last step reached.
 *   [0] magic "PSWPROG1"  [1] last code  [2] count  [3] TSC of the last code  [4..11] ring of the last 8 codes
 * Codes: 0x10xx HdmiProbeDxe, 0x11xx fan table, 0x12xx boot path (Ps5BootWindows). Never called at TPL_HIGH_LEVEL
 * inside an ICC transaction (WBINVD there would only cost time, but keep it out of timed windows). */
#ifndef PS5_PROGRESS_H
#define PS5_PROGRESS_H
#include <Library/BaseLib.h>

#define PS5_PROGRESS_CELL  0x111000400ULL
#define PS5_PROGRESS_MAGIC 0x31474f5250575350ULL   /* "PSWPROG1" little endian */

#define PS5_PROG_HDMI_ENTRY   0x1001u   /* HdmiProbeDxe entry (ICC mailbox found)          */
#define PS5_PROG_HDMI_PROBED  0x1002u   /* HDMI config/EDID probe done                     */
#define PS5_PROG_FAN_START    0x1101u   /* quiet servo table: before the first ICC message */
#define PS5_PROG_FAN_DONE     0x1102u   /* quiet servo table: finished (any result)        */
#define PS5_PROG_FAN_SKIPPED  0x1103u   /* quiet servo table: not written (Linux checks)   */
#define PS5_PROG_BDS          0x1201u   /* boot path entered                               */
#define PS5_PROG_DISPLAY      0x1202u   /* display started                                 */
#define PS5_PROG_LAUNCH       0x1203u   /* handing over to bootx64.efi                     */

#ifdef PS5_NO_PROGRESS_CELL   /* host unit tests: the cell address is not mapped there */
static inline void ps5_progress(UINT32 Code) { (void)Code; }
#else
static inline void ps5_progress(UINT32 Code)
{
  volatile UINT64 *P = (volatile UINT64 *)(UINTN)PS5_PROGRESS_CELL;
  UINT64 N = (P[0] == PS5_PROGRESS_MAGIC) ? P[2] : 0;
  if (P[0] != PS5_PROGRESS_MAGIC || Code == PS5_PROG_HDMI_ENTRY) { N = 0; for (UINTN I = 1; I < 12; I++) P[I] = 0; P[0] = PS5_PROGRESS_MAGIC; }
  P[1] = Code; P[2] = N + 1; P[3] = AsmReadTsc ();
  P[4 + (N & 7)] = Code;
  AsmWbinvd ();
}
#endif
#endif
