#!/usr/bin/env python3
"""lvgl-keys.py v1.1 (r5.25.4: + traffic-light colour of the system-check verdict line; v1.0 r5.25.4, 18.11.2026, user: in the Display tab the arrow keys opened the refresh-rate list instead of
switching the tab, then the menu froze; every control must first be selected with the arrows (highlighted) and only Enter
opens or changes it) - patches the copied LvglPkg/LvglDisplayEngineDxe/LvglFormRenderer.c after lvgl-tabs.py:
  - OnNavKey (widget level): while nothing is being edited, LEFT/RIGHT never reach the focused widget (LVGL's dropdown
    opens on RIGHT, a checkbox toggles on LEFT/RIGHT); UP/DOWN were already kept away from widgets; Enter opens/edits;
  - indev level: no tab switch while a dropdown list is open or a control is being edited (the tab switch rebuilt the
    screen under the open list - the freeze).
Usage: lvgl-keys.py <path to LvglFormRenderer.c>   (called by build-uefi.py --lvgl-ui)"""
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text()


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f'lvgl-keys: anchor found {s.count(old)}x: {old[:70]!r}')
    s = s.replace(old, new)


rep('''  (void)Ctx;

  if ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN) ||
      (Key == LV_KEY_NEXT) || (Key == LV_KEY_PREV))
  {''', '''  (void)Ctx;

  if ((Key == LV_KEY_LEFT) || (Key == LV_KEY_RIGHT)) {
    lv_event_stop_processing (Event);   // r5.25.4: arrows only select; Enter opens or changes a control
    return;
  }

  if ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN) ||
      (Key == LV_KEY_NEXT) || (Key == LV_KEY_PREV))
  {''')
rep('''  if (((Key == LV_KEY_LEFT) || (Key == LV_KEY_RIGHT)) && PswTabSwitch ((Key == LV_KEY_RIGHT) ? 1 : -1)) {''',
    '''  if (((Key == LV_KEY_LEFT) || (Key == LV_KEY_RIGHT)) &&
      !((Focused != NULL) && lv_obj_check_type (Focused, &lv_dropdown_class) && lv_dropdown_is_open (Focused)) &&
      !((mSession.Group != NULL) && lv_group_get_editing (mSession.Group)) &&   // r5.25.4: never under an open list / an edit
      PswTabSwitch ((Key == LV_KEY_RIGHT) ? 1 : -1)) {''')

# r5.25.4 (user: the traffic light showed only a dot, no colour): the system-check verdict line ("\u2022 GREEN ...",
# "\u2022 ORANGE ...", "\u2022 RED ...", always English, set by Ps5InfoDxe) is drawn in its colour
rep('''  PromptLabel = lv_label_create (Row);
  lv_label_set_text (PromptLabel, Prompt);
  lv_obj_set_flex_grow (PromptLabel, 1);
  LvglThemeApplyBodyFont (PromptLabel);
''', '''  PromptLabel = lv_label_create (Row);
  lv_label_set_text (PromptLabel, Prompt);
  lv_obj_set_flex_grow (PromptLabel, 1);
  LvglThemeApplyBodyFont (PromptLabel);
  if ((Prompt != NULL) && ((UINT8)Prompt[0] == 0xE2) && ((UINT8)Prompt[1] == 0x80) && ((UINT8)Prompt[2] == 0xA2) && (Prompt[3] == ' ')) {
    CONST CHAR8  *W = Prompt + 4;   // r5.25.4: system-check traffic light in colour
    if (AsciiStrnCmp (W, "GREEN", 5) == 0) {
      lv_obj_set_style_text_color (PromptLabel, lv_color_hex (0x3CC85A), 0);
    } else if (AsciiStrnCmp (W, "ORANGE", 6) == 0) {
      lv_obj_set_style_text_color (PromptLabel, lv_color_hex (0xF0A030), 0);
    } else if (AsciiStrnCmp (W, "RED", 3) == 0) {
      lv_obj_set_style_text_color (PromptLabel, lv_color_hex (0xE04848), 0);
    }
  }
''')
p.write_text(s)
print('lvgl-keys: patched', p)
