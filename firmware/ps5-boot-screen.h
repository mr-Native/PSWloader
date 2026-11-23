/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * r5.17 (17.11.2026, user request): PSWloader start screen, drawn straight into the GOP.
 * Dark vertical gradient, "PSWloader" large in the middle (HII system font glyphs scaled up), "v1.0" small below,
 * the key line ("ESC: UEFI BIOS - DEL: Boot Menu", German with lang=de) and a thin progress bar for the remaining
 * time. Only GOP Blt fills, no new font data. Falls back to nothing (the caller still runs the countdown) when the
 * HII font protocol is missing. */
#ifndef PS5_BOOT_SCREEN_H
#define PS5_BOOT_SCREEN_H
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiFont.h>

#define PS5_SCREEN_NAME    L"PSWloader"
#define PS5_SCREEN_VERSION L"v1.0.5"

STATIC EFI_GUID mPs5HiiFontGuid = { 0xe9ca4775, 0x8657, 0x47fc, { 0x97, 0xe7, 0x7e, 0xd6, 0x5a, 0x08, 0x43, 0x24 } };

STATIC VOID Ps5ScreenFill(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, UINT8 R, UINT8 G, UINT8 B, UINTN X, UINTN Y, UINTN W, UINTN H) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL P;
  if (W == 0 || H == 0) return;
  P.Red = R; P.Green = G; P.Blue = B; P.Reserved = 0;
  Gop->Blt(Gop, &P, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}

/* Width in pixels of Text at Scale (narrow system glyphs are EFI_GLYPH_WIDTH = 8 wide). */
STATIC UINTN Ps5ScreenTextWidth(CONST CHAR16 *Text, UINTN Scale) {
  UINTN N = 0;
  while (Text[N] != 0) N++;
  return N * EFI_GLYPH_WIDTH * Scale;
}

/* Draws Text with its top-left corner at (X, Y), every set glyph pixel as a Scale x Scale block. */
STATIC VOID Ps5ScreenText(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, EFI_HII_FONT_PROTOCOL *Font, CONST CHAR16 *Text,
                          UINTN X, UINTN Y, UINTN Scale, UINT8 R, UINT8 G, UINT8 B) {
  for (UINTN C = 0; Text[C] != 0; C++) {
    EFI_IMAGE_OUTPUT *Blt = NULL;
    UINTN Baseline = 0;
    if (EFI_ERROR(Font->GetGlyph(Font, Text[C], NULL, &Blt, &Baseline)) || Blt == NULL) continue;
    if (Blt->Image.Bitmap != NULL) {
      for (UINTN Py = 0; Py < Blt->Height; Py++) {
        for (UINTN Px = 0; Px < Blt->Width; Px++) {
          EFI_GRAPHICS_OUTPUT_BLT_PIXEL *S = &Blt->Image.Bitmap[Py * Blt->Width + Px];
          if ((UINTN)S->Red + S->Green + S->Blue > 3 * 128)
            Ps5ScreenFill(Gop, R, G, B, X + (C * EFI_GLYPH_WIDTH + Px) * Scale, Y + Py * Scale, Scale, Scale);
        }
      }
      FreePool(Blt->Image.Bitmap);
    }
    FreePool(Blt);
  }
}

STATIC UINTN mPs5ScreenBarX, mPs5ScreenBarY, mPs5ScreenBarW, mPs5ScreenBarH;

/* The whole screen; returns FALSE when nothing could be drawn. */
STATIC BOOLEAN Ps5BootScreenDraw(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, BOOLEAN German) {
  EFI_HII_FONT_PROTOCOL *Font = NULL;
  UINTN W, H, Big, Small, Hint, Y;
  CONST CHAR16 *Keys = German ? L"ESC: UEFI-BIOS  \x00b7  ENTF: Bootmen\x00fc" : L"ESC: UEFI BIOS  \x00b7  DEL: Boot Menu";
  if (Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) return FALSE;
  W = Gop->Mode->Info->HorizontalResolution;
  H = Gop->Mode->Info->VerticalResolution;
  if (W < 640 || H < 400) return FALSE;
  if (gST->ConOut != NULL) { gST->ConOut->EnableCursor(gST->ConOut, FALSE); gST->ConOut->ClearScreen(gST->ConOut); }
  /* background: deep blue-black gradient, 64 bands */
  for (UINTN I = 0; I < 64; I++) {
    UINTN Y0 = H * I / 64, Y1 = H * (I + 1) / 64;
    Ps5ScreenFill(Gop, (UINT8)(6 + I / 8), (UINT8)(10 + I / 5), (UINT8)(22 + I / 2), 0, Y0, W, Y1 - Y0);
  }
  if (EFI_ERROR(gBS->LocateProtocol(&mPs5HiiFontGuid, NULL, (VOID **)&Font)) || Font == NULL) return FALSE;
  Big = H / 128; if (Big < 4) Big = 4;              /* 1440 lines: 11 -> 88 x 209 px per letter */
  Small = Big / 3; if (Small < 2) Small = 2;
  Hint = Big / 4; if (Hint < 2) Hint = 2;
  Y = H / 2 - EFI_GLYPH_HEIGHT * Big * 3 / 4;
  /* title with a cyan glow offset behind it */
  Ps5ScreenText(Gop, Font, PS5_SCREEN_NAME, (W - Ps5ScreenTextWidth(PS5_SCREEN_NAME, Big)) / 2 + Big / 2, Y + Big / 2, Big, 0x00, 0x8C, 0xC8);
  Ps5ScreenText(Gop, Font, PS5_SCREEN_NAME, (W - Ps5ScreenTextWidth(PS5_SCREEN_NAME, Big)) / 2, Y, Big, 0xF4, 0xF8, 0xFF);
  Y += EFI_GLYPH_HEIGHT * Big + Big;
  /* accent line */
  Ps5ScreenFill(Gop, 0x00, 0xC8, 0xFF, W / 2 - W / 10, Y, W / 5, Small > 2 ? Small / 2 : 1);
  Y += Big;
  Ps5ScreenText(Gop, Font, PS5_SCREEN_VERSION, (W - Ps5ScreenTextWidth(PS5_SCREEN_VERSION, Small)) / 2, Y, Small, 0x9A, 0xA8, 0xC0);
  Y += EFI_GLYPH_HEIGHT * Small + Big * 2;
  Ps5ScreenText(Gop, Font, Keys, (W - Ps5ScreenTextWidth(Keys, Hint)) / 2, Y, Hint, 0xC8, 0xD2, 0xE4);
  Y += EFI_GLYPH_HEIGHT * Hint + Big;
  mPs5ScreenBarW = W / 4; mPs5ScreenBarX = (W - mPs5ScreenBarW) / 2; mPs5ScreenBarY = Y; mPs5ScreenBarH = Hint;
  Ps5ScreenFill(Gop, 0x1C, 0x26, 0x3C, mPs5ScreenBarX, mPs5ScreenBarY, mPs5ScreenBarW, mPs5ScreenBarH);
  return TRUE;
}

/* Remaining-time bar: Permille of the countdown already elapsed. */
STATIC VOID Ps5BootScreenProgress(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, UINTN Permille) {
  if (Gop == NULL || mPs5ScreenBarW == 0) return;
  if (Permille > 1000) Permille = 1000;
  Ps5ScreenFill(Gop, 0x00, 0xC8, 0xFF, mPs5ScreenBarX, mPs5ScreenBarY, mPs5ScreenBarW * Permille / 1000, mPs5ScreenBarH);
}
#endif
