#!/usr/bin/env python3
"""lvgl-ordered-grab.py v2.2 (r5.25.4: numbers without lv_label_set_text_fmt - froze the Boot tab; v2.1 r5.25.3: position numbers "1." "2." ... in front of the rows, renumbered after every move; v2.0 r5.23, 17.11.2026; user: no up/down buttons, Enter picks the disk, arrows move it, Enter
drops AND saves at once, Esc cancels, a mouse click = Enter; v1.1 r5.21d / v1.0 r5.21c) - patches the copied
LvglPkg/LvglDisplayEngineDxe/LvglFormRenderer.c (lvgl-uefi) for ordered lists (Boot tab "Boot order"):
  - the up/down buttons per row are gone; every row is focusable (blue when focused);
  - Enter (or a mouse click) on a row grabs it (orange); Up/Down move that row object inside the panel - no form
    rebuild while moving, so the focus stays on the moved row; the arrows do not move the focus while a row is grabbed;
  - Enter (or a click) drops it: only if the order changed, the new order is submitted once (the firmware callback
    writes Ps5BootListSel and \\PS5UEFI.CFG right away); after the rebuild the focus is on the dropped row;
  - Esc puts the row back where it was (nothing submitted);
  - any rebuild while a row is grabbed (refresh, form change) cancels the grab first (no stale object pointers).
Log lines (DEBUG): "LVGL ordered list: row N grabbed / A -> B / dropped at N (changed|unchanged) / cancelled".
Usage: lvgl-ordered-grab.py <path to LvglFormRenderer.c>   (called by build-uefi.py --lvgl-ui)"""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text()


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f'lvgl-ordered-grab: anchor found {s.count(old)}x: {old[:70]!r}')
    s = s.replace(old, new)


# 1) state + row handlers, before the popup handler (r5.25.3: + OrderedRenumber)
rep('''/**
  Popup button click handler -- records the chosen action and dismisses overlay.
**/''', '''//
// r5.23: ordered lists - Enter/click grabs a row, Up/Down move it locally, Enter/click drops + submits, Esc restores.
//
typedef struct {
  FORM_DISPLAY_ENGINE_STATEMENT  *Statement;
  UINT8                          ValueType;
  UINTN                          ActiveCount;
} LVGL_ORDERED_ROW_CTX;

STATIC struct {
  BOOLEAN                        Active;
  FORM_DISPLAY_ENGINE_STATEMENT  *Statement;
  lv_obj_t                       *Row;
  UINTN                          OrigIndex;
  UINTN                          Index;
} mGrab;
STATIC FORM_DISPLAY_ENGINE_STATEMENT  *mDropStatement;   // after a submit: focus this list row again
STATIC UINTN                          mDropIndex;
STATIC lv_obj_t                       *mDropFocusObj;

// r5.25.4: "N." without printf-style varargs (lv_label_set_text_fmt froze the Boot tab - LVGL/EDK2 varargs)
STATIC
VOID
OrderedSetNumber (
  lv_obj_t  *Label,
  UINT32    N
  )
{
  CHAR8  B[8];
  UINTN  i = 0;

  if (N >= 10) {
    B[i++] = (CHAR8)('0' + (N / 10) % 10);
  }

  B[i++] = (CHAR8)('0' + N % 10);
  B[i++] = '.';
  B[i]   = 0;
  lv_label_set_text (Label, B);
}

// r5.25.3: rows are the panel children 1..n (child 0 = header); child 0 of a row is its position label
STATIC
VOID
OrderedRenumber (
  lv_obj_t  *Row
  )
{
  lv_obj_t  *Panel = (Row != NULL) ? lv_obj_get_parent (Row) : NULL;
  uint32_t  n, k;

  if (Panel == NULL) {
    return;
  }

  n = lv_obj_get_child_count (Panel);
  for (k = 1; k < n; k++) {
    lv_obj_t  *R   = lv_obj_get_child (Panel, (int32_t)k);
    lv_obj_t  *Num = (R != NULL) ? lv_obj_get_child (R, 0) : NULL;
    if ((Num != NULL) && lv_obj_check_type (Num, &lv_label_class)) {
      OrderedSetNumber (Num, (UINT32)k);
    }
  }
}

STATIC
VOID
GrabStyle (
  lv_obj_t  *Row,
  BOOLEAN   On
  )
{
  lv_obj_set_style_bg_color (Row, lv_color_hex (On ? 0xE08A1E : THEME_COLOR_HIGHLIGHT_ROW), LV_STATE_FOCUSED);
  lv_obj_set_style_bg_color (Row, lv_color_hex (0xE08A1E), 0);
  lv_obj_set_style_bg_opa (Row, On ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

STATIC
VOID
GrabStart (
  lv_obj_t              *Row,
  LVGL_ORDERED_ROW_CTX  *Ctx
  )
{
  mGrab.Active    = TRUE;
  mGrab.Statement = Ctx->Statement;
  mGrab.Row       = Row;
  mGrab.Index     = (UINTN)lv_obj_get_index (Row) - 1;   // child 0 of the panel is the header label
  mGrab.OrigIndex = mGrab.Index;
  GrabStyle (Row, TRUE);
  lv_group_focus_obj (Row);
  DEBUG ((DEBUG_ERROR, "LVGL ordered list: row %u grabbed\\n", (UINT32)mGrab.Index));
}

STATIC
VOID
GrabMove (
  LVGL_ORDERED_ROW_CTX  *Ctx,
  BOOLEAN               Up
  )
{
  UINTN  j;

  if ((Up && (mGrab.Index == 0)) || (!Up && (mGrab.Index + 1 >= Ctx->ActiveCount))) {
    return;
  }

  j = Up ? mGrab.Index - 1 : mGrab.Index + 1;
  lv_obj_move_to_index (mGrab.Row, (int32_t)(j + 1));
  OrderedRenumber (mGrab.Row);   // r5.25.3
  lv_obj_scroll_to_view_recursive (mGrab.Row, LV_ANIM_OFF);
  DEBUG ((DEBUG_ERROR, "LVGL ordered list: row %u -> %u\\n", (UINT32)mGrab.Index, (UINT32)j));
  mGrab.Index = j;
}

STATIC
VOID
GrabDrop (
  LVGL_ORDERED_ROW_CTX  *Ctx
  )
{
  FORM_DISPLAY_ENGINE_STATEMENT  *Statement;
  UINT8                          *NewBuf;
  UINT64                         Moved;
  UINTN                          k;

  Statement = mGrab.Statement;
  DEBUG ((DEBUG_ERROR, "LVGL ordered list: dropped at %u (%a)\\n", (UINT32)mGrab.Index,
          (mGrab.Index != mGrab.OrigIndex) ? "changed" : "unchanged"));
  GrabStyle (mGrab.Row, FALSE);
  if (mGrab.Index != mGrab.OrigIndex) {
    NewBuf = AllocateCopyPool (Statement->CurrentValue.BufferLen, Statement->CurrentValue.Buffer);
    if (NewBuf != NULL) {
      Moved = GetArrayData (NewBuf, Ctx->ValueType, mGrab.OrigIndex);
      if (mGrab.Index < mGrab.OrigIndex) {
        for (k = mGrab.OrigIndex; k > mGrab.Index; k--) {
          SetArrayData (NewBuf, Ctx->ValueType, k, GetArrayData (NewBuf, Ctx->ValueType, k - 1));
        }
      } else {
        for (k = mGrab.OrigIndex; k < mGrab.Index; k++) {
          SetArrayData (NewBuf, Ctx->ValueType, k, GetArrayData (NewBuf, Ctx->ValueType, k + 1));
        }
      }

      SetArrayData (NewBuf, Ctx->ValueType, mGrab.Index, Moved);
      mDropStatement = Statement;
      mDropIndex     = mGrab.Index;
      mSession.UserInput->SelectedStatement    = Statement;
      mSession.UserInput->InputValue.Type      = EFI_IFR_TYPE_BUFFER;
      mSession.UserInput->InputValue.Buffer    = NewBuf;
      mSession.UserInput->InputValue.BufferLen = Statement->CurrentValue.BufferLen;
      mSession.UserInput->Action               = 0;
      mSession.ExitRequested                   = TRUE;
    }
  }

  ZeroMem (&mGrab, sizeof (mGrab));
}

STATIC
VOID
GrabCancel (
  BOOLEAN  Visual
  )
{
  if (mGrab.Active && Visual) {
    lv_obj_move_to_index (mGrab.Row, (int32_t)(mGrab.OrigIndex + 1));
    OrderedRenumber (mGrab.Row);   // r5.25.3
    GrabStyle (mGrab.Row, FALSE);
    lv_group_focus_obj (mGrab.Row);
    DEBUG ((DEBUG_ERROR, "LVGL ordered list: cancelled, row back at %u\\n", (UINT32)mGrab.OrigIndex));
  }

  ZeroMem (&mGrab, sizeof (mGrab));
}

/** Enter (pick/drop) for the row under the keypad focus or the mouse. */
STATIC
VOID
OrderedRowEnter (
  lv_obj_t              *Row,
  LVGL_ORDERED_ROW_CTX  *Ctx
  )
{
  if (!mGrab.Active) {
    GrabStart (Row, Ctx);
  } else if (mGrab.Statement == Ctx->Statement) {
    GrabDrop (Ctx);
  }
}

STATIC
BOOLEAN
OrderedRowUsable (
  LVGL_ORDERED_ROW_CTX  *Ctx
  )
{
  return (Ctx != NULL) && (mSession.UserInput != NULL) && (mPopupOverlay == NULL) && IsStatementInCurrentForm (Ctx->Statement);
}

STATIC
VOID
OnOrderedRowKey (
  lv_event_t  *Event
  )
{
  LVGL_ORDERED_ROW_CTX  *Ctx;
  lv_key_t              Key;

  Ctx = (LVGL_ORDERED_ROW_CTX *)lv_event_get_user_data (Event);
  if (!OrderedRowUsable (Ctx)) {
    return;
  }

  Key = lv_event_get_key (Event);
  if (Key == LV_KEY_ENTER) {
    OrderedRowEnter (lv_event_get_current_target_obj (Event), Ctx);
    lv_event_stop_processing (Event);
  } else if (mGrab.Active && (mGrab.Statement == Ctx->Statement)) {
    if ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN)) {
      GrabMove (Ctx, Key == LV_KEY_UP);
      lv_event_stop_processing (Event);
    } else if (Key == LV_KEY_ESC) {
      GrabCancel (TRUE);
      lv_event_stop_processing (Event);
    }
  }
}

/** Mouse/touch click = Enter. The keypad's own CLICKED after Enter is ignored (handled as LV_EVENT_KEY above). */
STATIC
VOID
OnOrderedRowClick (
  lv_event_t  *Event
  )
{
  LVGL_ORDERED_ROW_CTX  *Ctx;
  lv_indev_t            *Indev;

  Ctx   = (LVGL_ORDERED_ROW_CTX *)lv_event_get_user_data (Event);
  Indev = lv_indev_active ();
  if (!OrderedRowUsable (Ctx) || (Indev == NULL) || (lv_indev_get_type (Indev) != LV_INDEV_TYPE_POINTER)) {
    return;
  }

  if (mGrab.Active && (mGrab.Statement == Ctx->Statement)) {
    GrabDrop (Ctx);   // a click anywhere in the list drops the grabbed row where it is now
  } else if (!mGrab.Active) {
    OrderedRowEnter (lv_event_get_current_target_obj (Event), Ctx);
  }
}

/**
  Popup button click handler -- records the chosen action and dismisses overlay.
**/''')

# 2) widget: no up/down buttons, the row itself is the control
rep('''  lv_obj_t                 *UpBtn;
  lv_obj_t                 *DownBtn;
  lv_obj_t                 *BtnLabel;
  LIST_ENTRY               *Link;''', '''  LIST_ENTRY               *Link;''')
rep('''  LVGL_ORDERED_MOVE_CTX    *UpCtx;
  LVGL_ORDERED_MOVE_CTX    *DownCtx;
  BOOLEAN                  Grayout;

  if (IsListEmpty (&Statement->OptionListHead)) {''', '''  BOOLEAN                  Grayout;

  (VOID)OnOrderedListMove;   // r5.23: the up/down buttons are gone (handler kept unused)
  if (IsListEmpty (&Statement->OptionListHead)) {''')
rep('''    Row = lv_obj_create (Panel);
    lv_obj_set_size (Row, LV_PCT (100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow (Row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all (Row, THEME_PAD_ROW_TIGHT, 0);
    lv_obj_set_style_border_width (Row, 0, 0);
    lv_obj_set_style_bg_opa (Row, LV_OPA_TRANSP, 0);
''', '''    Row = lv_obj_create (Panel);
    lv_obj_set_size (Row, LV_PCT (100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow (Row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all (Row, THEME_PAD_ROW_TIGHT, 0);
    lv_obj_set_style_border_width (Row, 0, 0);
    lv_obj_set_style_bg_opa (Row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag (Row, LV_OBJ_FLAG_SCROLLABLE);
    {   // r5.25.3 (user: a real boot table): position number in front of every drive, renumbered after each move
      lv_obj_t  *Num = lv_label_create (Row);
      OrderedSetNumber (Num, (UINT32)(i + 1));
      lv_obj_set_width (Num, 44);
      LvglThemeApplyBodyFont (Num);
      lv_obj_set_style_text_color (Num, lv_color_hex (THEME_COLOR_TEXT_PRIMARY), 0);   // r5.25.4: user - the numbers were too dark; same colour as the drive name
    }
    if (!Grayout) {   // r5.23: the row itself takes the focus; Enter / a click picks it up
      LVGL_ORDERED_ROW_CTX  *RowCtx = AllocateZeroPool (sizeof (LVGL_ORDERED_ROW_CTX));

      lv_obj_set_style_bg_opa (Row, LV_OPA_COVER, LV_STATE_FOCUSED);
      lv_obj_set_style_bg_color (Row, lv_color_hex (THEME_COLOR_HIGHLIGHT_ROW), LV_STATE_FOCUSED);
      if (RowCtx != NULL) {
        RowCtx->Statement   = Statement;
        RowCtx->ValueType   = ValueType;
        RowCtx->ActiveCount = ActiveCount;
        lv_obj_add_event_cb (Row, OnOrderedRowKey, LV_EVENT_KEY | LV_EVENT_PREPROCESS, RowCtx);
        lv_obj_add_event_cb (Row, OnOrderedRowClick, LV_EVENT_CLICKED, RowCtx);
      }

      AddToNavGroup (Group, Row, NULL);
      if ((i == 0) && (mNavCount > 0)) {
        mNavStatement[mNavCount - 1] = Statement;   // r5.21d: the browser highlight focuses the list, not a later item
      }

      if ((mDropStatement == Statement) && (mDropIndex == i)) {
        mDropFocusObj = Row;                          // r5.23: focus the dropped row after the rebuild
      }
    }
''')
i = s.index('''    //
    // Up button.
    //
    UpBtn    = lv_btn_create (Row);''')
j = s.index('''      AddToNavGroup (Group, DownBtn, NULL);
    }
''', i) + len('''      AddToNavGroup (Group, DownBtn, NULL);
    }
''')
s = s[:i] + s[j:]

# 3) while a row is grabbed, the indev-level navigation must not touch these keys
rep('''  Focused = (mSession.Group != NULL) ? lv_group_get_focused (mSession.Group) : NULL;

  //
  // UP/DOWN: walk mNavList''', '''  Focused = (mSession.Group != NULL) ? lv_group_get_focused (mSession.Group) : NULL;

  if (mGrab.Active && ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN) || (Key == LV_KEY_ESC) || (Key == LV_KEY_ENTER) ||
                       (Key == LV_KEY_LEFT) || (Key == LV_KEY_RIGHT) || (Key == LV_KEY_NEXT) || (Key == LV_KEY_PREV))) {
    return;   // r5.23: the grabbed ordered-list row owns the keys (no tab switch, no focus walk)
  }

  //
  // UP/DOWN: walk mNavList''')

# 4) a rebuild cancels any grab (row objects are recreated); after a drop the moved row gets the focus
rep('''  //
  // Load the screen.
  //
  lv_screen_load (mSession.Screen);
''', '''  GrabCancel (FALSE);   // r5.23: rows were recreated - a grab from the old screen ends (nothing was submitted)

  //
  // Load the screen.
  //
  lv_screen_load (mSession.Screen);
''')
rep('''  lv_obj_update_layout (mSession.Screen);
  if (mRestoreFocusPending) {''', '''  lv_obj_update_layout (mSession.Screen);
  mDropStatement = NULL;
  if (mDropFocusObj != NULL) {   // r5.23: the dropped ordered-list row keeps the focus
    lv_group_focus_obj (mDropFocusObj);
    mDropFocusObj        = NULL;
    mRestoreFocusPending = FALSE;
  } else if (mRestoreFocusPending) {''')
p.write_text(s)
print('lvgl-ordered-grab: patched', p)
