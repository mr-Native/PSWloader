#!/usr/bin/env python3
"""lvgl-tabs.py v1.1 (r5.25.3: the front page is never shown - ESC from a tab lands on Overview, leave the setup with Save & Exit; v1.0 r5.22, 17.11.2026, user: "UEFI structured and clean", PROPOSAL uefi-gui) - patches the copied
LvglPkg/LvglDisplayEngineDxe/LvglFormRenderer.c (lvgl-uefi) for the six PSWloader tabs:
  - entering the setup opens the first tab (Overview) directly instead of the front-page list;
  - Left/Right on the main form of a tab switch to the neighbouring tab (Overview, Performance & Fan, Display, Boot,
    Diagnostics, Save & Exit); the browser goes back to the front page and the renderer selects the target tab's
    goto there at once, so the front page is not shown in between;
  - ESC from a tab returns to the front page, which at once opens Overview again (r5.25.3; was: the hub list).
Tab order = PSW_TAB_ID (last GUID byte of the PSWloader formsets 9c1e5f2a-1360-4e1b-b05e-13600ef100xx).
Usage: lvgl-tabs.py <path to LvglFormRenderer.c>   (called by build-uefi.py --lvgl-ui)"""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text()


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f'lvgl-tabs: anchor found {s.count(old)}x: {old[:70]!r}')
    s = s.replace(old, new)


# 1) state + helpers before the generic click handler (needs IsFrontPageForm/ApplyUserInput declared above it)
rep('''/**
  Generic click handler -- records the statement selection and requests exit.
**/''', '''//
// r5.22: PSWloader tabs (lvgl-tabs.py)
//
STATIC CONST UINT8     PSW_TAB_ID[6] = { 0x01, 0x0a, 0x10, 0x03, 0x12, 0x0c };
STATIC CONST EFI_GUID  mPswTabBase   = { 0x9c1e5f2a, 0x1360, 0x4e1b, { 0xb0, 0x5e, 0x13, 0x60, 0x0e, 0xf1, 0x00, 0x00 } };
STATIC BOOLEAN         mPswTabSeen;      // a form was shown in this browser session (reset in LvglRendererCleanup)
STATIC BOOLEAN         mPswTabPending;
STATIC EFI_GUID        mPswTabTarget;
STATIC BOOLEAN         mPswOncePending;   // r5.25.4: DEL - open the Boot tab, then its "Boot once from ..." page
STATIC EFI_GUID        mPswOnceTarget;

STATIC
INTN
PswTabIndex (
  CONST EFI_GUID  *Guid
  )
{
  if ((Guid == NULL) || (CompareMem (Guid, &mPswTabBase, 15) != 0)) {
    return -1;
  }

  for (UINTN T = 0; T < 6; T++) {
    if (((CONST UINT8 *)Guid)[15] == PSW_TAB_ID[T]) {
      return (INTN)T;
    }
  }

  return -1;
}

/**
  Left/Right on the main form of a PSWloader tab: remember the neighbour, leave to the front page.
**/
STATIC
BOOLEAN
PswTabSwitch (
  INTN  Dir
  )
{
  INTN  Idx;

  if ((mSession.FormData == NULL) || (mSession.FormData->FormId != 1) || (mSession.UserInput == NULL)) {
    return FALSE;
  }

  if ((mSession.Group != NULL) && lv_group_get_editing (mSession.Group)) {
    return FALSE;
  }

  Idx = PswTabIndex (&mSession.FormData->FormSetGuid);
  if ((Idx < 0) || (Idx + Dir < 0) || (Idx + Dir > 5)) {
    return (Idx >= 0);   // first/last tab: swallow the key
  }

  CopyGuid (&mPswTabTarget, &mPswTabBase);
  ((UINT8 *)&mPswTabTarget)[15] = PSW_TAB_ID[Idx + Dir];
  mPswTabPending = TRUE;
  DEBUG ((DEBUG_ERROR, "PSW tabs: %d -> %d\\n", (INT32)Idx, (INT32)(Idx + Dir)));
  RequestUserExit (BROWSER_ACTION_FORM_EXIT, 0, NULL);
  return TRUE;
}

/**
  On the front page: open the pending tab, or the first tab when the setup was just entered.
**/
STATIC
BOOLEAN
PswTabAutoGoto (
  VOID
  )
{
  LIST_ENTRY                     *Link;
  FORM_DISPLAY_ENGINE_STATEMENT  *Statement;
  EFI_GUID                       Target;
  BOOLEAN                        Fresh;
  STATIC EFI_GUID                PswPresetGuid = { 0x5efc5ae0, 0xb753, 0x4f7a, { 0xa6, 0x62, 0xe3, 0x4d, 0x15, 0xa6, 0xd8, 0xe5 } };

  Fresh       = !mPswTabSeen;
  mPswTabSeen = TRUE;
  (VOID)Fresh;   // r5.25.3 (user: no front page at all): every visit of the front page goes straight to a tab
  if (mPswOncePending && !IsFrontPageForm () && (mSession.UserInput != NULL)) {   // r5.25.4: second step of DEL
    mPswOncePending = FALSE;
    for (Link = mSession.FormData->StatementListHead.ForwardLink;
         Link != &mSession.FormData->StatementListHead;
         Link = Link->ForwardLink)
    {
      Statement = FORM_DISPLAY_ENGINE_STATEMENT_FROM_LINK (Link);
      if ((Statement->OpCode != NULL) && (Statement->OpCode->OpCode == EFI_IFR_REF_OP) &&
          (Statement->OpCode->Length >= sizeof (EFI_IFR_REF3)) &&
          CompareGuid (&((EFI_IFR_REF3 *)Statement->OpCode)->FormSetId, &mPswOnceTarget))
      {
        RequestUserExit (0, 0, Statement);
        return TRUE;
      }
    }
    return FALSE;
  }

  if (!IsFrontPageForm () || (mSession.UserInput == NULL)) {
    return FALSE;
  }

  if (mPswTabPending) {
    CopyGuid (&Target, &mPswTabTarget);
  } else {
    CopyGuid (&Target, &mPswTabBase);
    ((UINT8 *)&Target)[15] = PSW_TAB_ID[0];
    if (Fresh) {   // r5.25.4: DEL on the start screen asks for its page (volatile Ps5SetupTarget = formset GUID, read once)
      UINTN  TargetSize = sizeof (EFI_GUID);
      if (!EFI_ERROR (gRT->GetVariable (L"Ps5SetupTarget", &PswPresetGuid, NULL, &TargetSize, &Target)) && (TargetSize == sizeof (EFI_GUID))) {
        gRT->SetVariable (L"Ps5SetupTarget", &PswPresetGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS, 0, NULL);
        if (CompareMem (&Target, &mPswTabBase, 15) != 0) {   // a page inside a tab (HW 18.11.: not on the front page) - Boot tab first
          CopyGuid (&mPswOnceTarget, &Target);
          mPswOncePending = TRUE;
          CopyGuid (&Target, &mPswTabBase);
          ((UINT8 *)&Target)[15] = PSW_TAB_ID[3];
        }
      } else {
        CopyGuid (&Target, &mPswTabBase);
        ((UINT8 *)&Target)[15] = PSW_TAB_ID[0];
      }
    } else {   // r5.25.4: boot menu (Ps5OnceMode): back on the front page = its boot did not start or ESC - leave the setup
      UINT8  Once = 0;
      UINTN  OnceSize = sizeof (Once);
      if (!EFI_ERROR (gRT->GetVariable (L"Ps5OnceMode", &PswPresetGuid, NULL, &OnceSize, &Once)) && Once) {
        RequestUserExit (BROWSER_ACTION_FORM_EXIT, 0, NULL);
        return TRUE;
      }
    }
  }

  mPswTabPending = FALSE;
  for (Link = mSession.FormData->StatementListHead.ForwardLink;
       Link != &mSession.FormData->StatementListHead;
       Link = Link->ForwardLink)
  {
    Statement = FORM_DISPLAY_ENGINE_STATEMENT_FROM_LINK (Link);
    if ((Statement->OpCode != NULL) && (Statement->OpCode->OpCode == EFI_IFR_REF_OP) &&
        (Statement->OpCode->Length >= sizeof (EFI_IFR_REF3)) &&
        CompareGuid (&((EFI_IFR_REF3 *)Statement->OpCode)->FormSetId, &Target))
    {
      RequestUserExit (0, 0, Statement);
      return TRUE;
    }
  }

  if (CompareMem (&Target, &mPswTabBase, 15) != 0) {   // r5.25.4: requested page not on the front page - Boot tab instead
    CopyGuid (&Target, &mPswTabBase);
    ((UINT8 *)&Target)[15] = PSW_TAB_ID[3];
    for (Link = mSession.FormData->StatementListHead.ForwardLink;
         Link != &mSession.FormData->StatementListHead;
         Link = Link->ForwardLink)
    {
      Statement = FORM_DISPLAY_ENGINE_STATEMENT_FROM_LINK (Link);
      if ((Statement->OpCode != NULL) && (Statement->OpCode->OpCode == EFI_IFR_REF_OP) &&
          (Statement->OpCode->Length >= sizeof (EFI_IFR_REF3)) &&
          CompareGuid (&((EFI_IFR_REF3 *)Statement->OpCode)->FormSetId, &Target))
      {
        RequestUserExit (0, 0, Statement);
        return TRUE;
      }
    }
  }

  return FALSE;
}

/**
  Generic click handler -- records the statement selection and requests exit.
**/''')

# 2) Left/Right in the indev-level key handler (after the popup check)
rep('''  //
  // UP/DOWN: walk mNavList (so grayed rows stay reachable).''', '''  if (((Key == LV_KEY_LEFT) || (Key == LV_KEY_RIGHT)) && PswTabSwitch ((Key == LV_KEY_RIGHT) ? 1 : -1)) {
    return;   // r5.22: tab switch
  }

  //
  // UP/DOWN: walk mNavList (so grayed rows stay reachable).''')

# 3) before the event loop: auto-open on the front page
rep('''  //
  // LVGL event loop -- run until user makes a selection or presses ESC.
  //''', '''  if (PswTabAutoGoto ()) {   // r5.22: front page -> first / requested tab without showing the list
    lv_uefi_keypad_drain ();
    return EFI_SUCCESS;
  }

  //
  // LVGL event loop -- run until user makes a selection or presses ESC.
  //''')

# 4) new browser session -> open the first tab again
rep('''LvglRendererCleanup (
  VOID
  )
{
  AptioChromeTeardown ();
''', '''LvglRendererCleanup (
  VOID
  )
{
  mPswTabSeen    = FALSE;   // r5.22: the next setup session starts on the first tab
  mPswTabPending = FALSE;
  AptioChromeTeardown ();
''')
p.write_text(s)
print('lvgl-tabs: patched', p)
