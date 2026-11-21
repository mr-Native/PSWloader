#!/usr/bin/env python3
"""lvgl-lang.py v1.1 (r5.25.4: header clock hidden; v1.0 r5.25.3, 18.11.2026, user: after a language change every page and every text must switch) - patches
the copied LvglPkg/LvglDisplayEngineDxe/{LvglAptioChrome.c,LvglFormRenderer.c} (lvgl-uefi): the fixed English texts of the
renderer (hotkey bar, clock, unsaved-changes notice, save/defaults/discard dialogs, HII popup buttons) become German when
PlatformLang starts with "de". PlatformLang is read every time a text is drawn, so a language change shows at the next
redraw of the form (the language callback applies the form at once). Form texts themselves come from HII (en-US/de-DE).
Usage: lvgl-lang.py <LvglDisplayEngineDxe dir>   (called by build-uefi.py --lvgl-ui after lvgl-tabs.py)"""
import sys
from pathlib import Path

d = Path(sys.argv[1])
HELPER = '''
// r5.25.3 (lvgl-lang.py): German renderer texts when PlatformLang is de-*
#include <Library/UefiRuntimeServicesTableLib.h>
STATIC BOOLEAN
PswGerman (
  VOID
  )
{
  STATIC EFI_GUID  PswGlobalVar = { 0x8BE4DF61, 0x93CA, 0x11d2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
  CHAR8            Lang[8];
  UINTN            Size = sizeof (Lang);

  return !EFI_ERROR (gRT->GetVariable (L"PlatformLang", &PswGlobalVar, NULL, &Size, Lang)) && (Lang[0] == 'd');
}
#define PSW_T(En, De)  (PswGerman () ? (De) : (En))
'''


def u(t):   # German text -> C string literal with UTF-8 escapes (no non-ASCII in the source)
    out = ''
    for ch in t:
        if ord(ch) < 128:
            out += ch
        else:
            out += ''.join('\\x%02x' % b for b in ch.encode('utf-8')) + '" "'
    return '"' + out + '"'


def patch(name, after_include, pairs):
    p = d / name
    s = p.read_text()
    if after_include not in s:
        sys.exit(f'lvgl-lang: include anchor missing in {name}')
    s = s.replace(after_include, after_include + HELPER, 1)
    for old, en, de in pairs:
        if old not in s:
            sys.exit(f'lvgl-lang: anchor missing in {name}: {old[:60]!r}')
        s = s.replace(old, old.replace(f'"{en}"', f'PSW_T ("{en}", {u(de)})'))
    p.write_text(s)
    print('lvgl-lang: patched', p)


patch('LvglAptioChrome.c', '#include "LvglAptioChrome.h"\n', [
    ('AddHotKeyChip (Bar, LV_SYMBOL_UP LV_SYMBOL_DOWN, "Move");', 'Move', 'Bewegen'),
    ('AddHotKeyChip (Bar, "Enter", "Select");', 'Select', 'Ausw\u00e4hlen'),
    ('AddHotKeyChip (Bar, "Esc",   "Exit");', 'Exit', 'Zur\u00fcck'),
    ('AddHotKeyChip (Bar, "Power", "short: next  long: OK  double: back");', 'short: next  long: OK  double: back',
     'kurz: weiter  lang: OK  doppelt: zur\u00fcck'),
    ('AddHotKeyChip (Bar, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, "tabs");', 'tabs', 'Reiter'),
    ('lv_label_set_text (mClockLabel, "Time not set");', 'Time not set', 'Uhrzeit nicht gesetzt'),
])
# the notice has a symbol prefix: replace the whole call
p = d / 'LvglAptioChrome.c'; s = p.read_text()
old = 'lv_label_set_text (Notice, LV_SYMBOL_WARNING "  Unsaved changes -- press F10 to save");'
if old not in s:
    sys.exit('lvgl-lang: notice anchor missing')
s = s.replace(old, 'lv_label_set_text (Notice, PSW_T (LV_SYMBOL_WARNING "  Unsaved changes -- press F10 to save", LV_SYMBOL_WARNING '
              + u('  Ungespeicherte \u00c4nderungen -- F10 speichert') + '));')
p.write_text(s)

p = d / 'LvglFormRenderer.c'; s = p.read_text()
inc = '#include "LvglAptioChrome.h"\n'
if inc not in s:
    sys.exit('lvgl-lang: include anchor missing in LvglFormRenderer.c')
s = s.replace(inc, inc + HELPER, 1)
R = [
    ('ShowDiscard ? "You have unsaved changes." : "Save the current settings?",',
     'ShowDiscard ? PSW_T ("You have unsaved changes.", ' + u('Es gibt ungespeicherte \u00c4nderungen.') + ') : PSW_T ("Save the current settings?", ' + u('Aktuelle Einstellungen speichern?') + '),', 1),
    ('? "Load Defaults?" : "Save Changes?";', '? PSW_T ("Load Defaults?", ' + u('Standardwerte laden?') + ') : PSW_T ("Save Changes?", ' + u('\u00c4nderungen speichern?') + ');', 1),
    ('? "Load" : "Save";', '? PSW_T ("Load", "Laden") : PSW_T ("Save", "Speichern");', 1),
    ('lv_label_set_text (Lbl, "Discard (N)");', 'lv_label_set_text (Lbl, PSW_T ("Discard (N)", "Verwerfen (N)"));', 1),
    ('lv_label_set_text (Lbl, "Cancel (Esc)");', 'lv_label_set_text (Lbl, PSW_T ("Cancel (Esc)", "Abbrechen (Esc)"));', 1),
    ('ShowPopup (PopupGroup, "Unsaved Changes", "Save", BROWSER_ACTION_SUBMIT, TRUE);',
     'ShowPopup (PopupGroup, PSW_T ("Unsaved Changes", ' + u('Ungespeicherte \u00c4nderungen') + '), PSW_T ("Save", "Speichern"), BROWSER_ACTION_SUBMIT, TRUE);', 1),
    ('Group, "Cancel", EfiHiiPopupSelectionCancel);', 'Group, PSW_T ("Cancel", "Abbrechen"), EfiHiiPopupSelectionCancel);', 0),
    ('Group, "Yes (Y)", EfiHiiPopupSelectionYes);', 'Group, PSW_T ("Yes (Y)", "Ja (Y)"), EfiHiiPopupSelectionYes);', 0),
    ('Group, "No (N)", EfiHiiPopupSelectionNo);', 'Group, PSW_T ("No (N)", "Nein (N)"), EfiHiiPopupSelectionNo);', 0),
]
for old, new, exactly in R:
    n = s.count(old)
    if n == 0 or (exactly and n != exactly):
        sys.exit(f'lvgl-lang: anchor found {n}x in LvglFormRenderer.c: {old[:60]!r}')
    s = s.replace(old, new)
p.write_text(s)
print('lvgl-lang: patched', p)

# r5.25.4 (user 18.11.: time and date are not right, remove them if they cannot be determined exactly): the clock in the
# header is never shown - the console gives the firmware no reliable time (only the lab writes \PS5UTC.SEED)
p = d / 'LvglAptioChrome.c'; s = p.read_text()
old = '  mClockLabel = lv_label_create (Bar);\n'
if s.count(old) != 1:
    sys.exit('lvgl-lang: clock label anchor missing')
s = s.replace(old, old + '  lv_obj_add_flag (mClockLabel, LV_OBJ_FLAG_HIDDEN);   // r5.25.4: no clock (no reliable time source)\n')
p.write_text(s)
print('lvgl-lang: clock hidden')
