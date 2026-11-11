// ==WindhawkMod==
// @id              keyboard-layout-text-converter
// @name            Keyboard Layout Text Converter
// @description     Convert mistyped text in place with a shortcut, adapting to installed keyboard layouts
// @version         0.5.0
// @author          OMAR ALAFA
// @homepage        https://github.com/omar-devloop/keyboard-layout-text-converter
// @include         explorer.exe
// @compilerOptions -luser32 -lkernel32 -lshell32 -lgdi32
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Keyboard Layout Text Converter

Converts mistyped text directly in place using a global shortcut, across most
Windows applications.

## How it works
- Press the configured shortcut while text is selected (recommended), or use the
  last-word fallback mode. The default shortcut is left Ctrl + left Shift.
  A modifier-only shortcut only converts when the keys are pressed and released
  on their own; pressing another key (e.g. Ctrl+Shift+T) cancels it.
- The shortcut keypress is swallowed (for regular combos) and the conversion
  starts as soon as the keys are released.
- The mod copies the target text, detects the likely source/target keyboard
  layout among the keyboard layouts installed on your device, converts the text
  by key positions between layouts, then pastes the converted text back.
- By default the original clipboard text content is restored after replacement.
- A small success toast appears in the bottom-right screen corner after each
  conversion; it can be disabled in the settings.

## Examples
- Typed with English layout but meant as another installed layout (and back),
  depending on the active/installed layouts and text.
- Works best when at least two keyboard layouts/languages are installed.

## Notes and limitations
- Replacement is clipboard-based (copy/paste simulation), so behavior can vary
  in apps with custom clipboard/shortcut handling.
- The mod may not be able to interact with elevated (UIPI) windows, secure
  fields, or some terminals/remote apps.
- Very short or ambiguous text can be hard to auto-detect; use a longer
  selection or adjust settings if needed.
- Runs inside `explorer.exe` to provide a global shortcut and conversion worker.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hotkey: "LCtrl+LShift"
  $name: "Conversion shortcut"
  $description: "Global shortcut to convert text. The conversion starts when the shortcut keys are released. Supports modifier-only combinations (e.g. LCtrl+LShift) and regular combos (e.g. Ctrl+Alt+Space, Win+Shift+F12). Use L/R prefixes to pick a side."
- mode: auto
  $name: "Conversion mode"
  $options:
    - auto: "Auto-detect layouts"
    - cycle: "Cycle target among installed layouts"
  $description: "Auto-detect picks the most plausible layout mapping. Cycle tries the next installed layout."
- scope: selection-first
  $name: "Capture scope"
  $options:
    - selection-first: "Selection, fallback to previous word"
    - selection-only: "Selection only"
    - last-word: "Previous word only"
  $description: "What text to convert when the shortcut is pressed."
- restoreClipboard: true
  $name: "Restore clipboard"
  $description: "Restore previous clipboard Unicode text after conversion."
- showNotification: true
  $name: "Show notification"
  $description: "Show a small success toast in the screen corner after a successful conversion."
- switchLayout: false
  $name: "Switch keyboard layout after convert"
  $description: "Try to switch the active keyboard layout to the detected target layout."
- actionDelayMs: 40
  $name: "Clipboard timeout (ms)"
  $description: "Base timeout in milliseconds for clipboard copy/paste handshakes. Increase it if conversions fail in slow applications."
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void Dbg(const wchar_t* msg) {
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\n");
}

template <typename... Args>
void Logf(const wchar_t* fmt, Args... args) {
    wchar_t buf[512];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args...);
    Wh_Log(L"%s", buf);
    OutputDebugStringW(L"[KLTC] ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

constexpr UINT WM_APP_CONVERT = WM_APP + 0x51;

struct Hotkey {
    UINT modifiers = 0;  // MOD_* | MOD_NOREPEAT (we ignore norepeat in parsing)
    UINT vk = 0;         // 0 for modifier-only shortcuts such as LCtrl+LShift
    bool valid = false;
    bool modifierOnly = false;
    // Specific left/right modifier vks for modifier-only shortcuts, indexed by
    // class (0=ctrl, 1=alt, 2=shift, 3=win). 0 means "either side".
    UINT modVk[4] = {};
};

struct Settings {
    std::wstring hotkeyStr = L"LCtrl+LShift";
    Hotkey hotkey;
    std::wstring mode = L"auto";
    std::wstring scope = L"selection-first";
    bool restoreClipboard = true;
    bool switchLayout = false;
    bool showNotification = true;
    int actionDelayMs = 40;
};

struct KeyStroke {
    UINT vk = 0;
    UINT scan = 0;
    bool shift = false;
};

struct LayoutMap {
    HKL hkl = nullptr;
    std::wstring charsForKey[256][2];  // [vk][shift] -> produced char(s)
    std::unordered_map<wchar_t, KeyStroke> keyForChar;
    std::unordered_map<wchar_t, KeyStroke> keyForCharShift;
};

Settings g_settings;
std::mutex g_settingsMutex;
std::mutex g_convertMutex;
std::mutex g_layoutMutex;

HHOOK g_keyboardHook = nullptr;
HANDLE g_workerThread = nullptr;    // hook + message pump thread
DWORD g_workerThreadId = 0;
HANDLE g_convertThread = nullptr;   // conversion thread, kept separate so the
DWORD g_convertThreadId = 0;        // hook thread never blocks input dispatch
HANDLE g_stopEvent = nullptr;
ULONG_PTR g_cycleIndex = 0;
UINT g_suppressedHotkeyVk = 0;

// Combo-release tracking. Only touched on the hook thread.
bool g_keyDown[256] = {};
bool g_hotkeyPending = false;

std::vector<HKL> g_layouts;
std::unordered_map<UINT_PTR, LayoutMap> g_layoutMaps;

// Toast notification state (conversion thread only).
HWND g_toastHwnd = nullptr;
HFONT g_toastFont = nullptr;
ULONGLONG g_toastStartTick = 0;
bool g_toastClassRegistered = false;
wchar_t g_toastClass[64] = {};
HINSTANCE g_toastInstance = nullptr;

std::wstring Trim(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) a++;
    size_t b = s.size();
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}

UINT ParseVkToken(const std::wstring& tokenRaw) {
    std::wstring token = ToLower(Trim(tokenRaw));
    if (token.empty()) return 0;
    if (token.size() == 1) {
        wchar_t c = token[0];
        if (c >= L'a' && c <= L'z') return (UINT)(L'A' + (c - L'a'));
        if (c >= L'0' && c <= L'9') return (UINT)c;
    }
    if (token.rfind(L"0x", 0) == 0 || token.rfind(L"vk_", 0) == 0) {
        // try parse hex/decimal after prefix best-effort
    }
    if (token[0] >= L'0' && token[0] <= L'9') {
        return (UINT)wcstoul(token.c_str(), nullptr, 0);
    }
    if (token == L"space") return VK_SPACE;
    if (token == L"tab") return VK_TAB;
    if (token == L"enter" || token == L"return") return VK_RETURN;
    if (token == L"esc" || token == L"escape") return VK_ESCAPE;
    if (token == L"backspace" || token == L"bksp") return VK_BACK;
    if (token == L"delete" || token == L"del") return VK_DELETE;
    if (token == L"insert" || token == L"ins") return VK_INSERT;
    if (token == L"home") return VK_HOME;
    if (token == L"end") return VK_END;
    if (token == L"pageup" || token == L"pgup") return VK_PRIOR;
    if (token == L"pagedown" || token == L"pgdn") return VK_NEXT;
    if (token == L"left") return VK_LEFT;
    if (token == L"right") return VK_RIGHT;
    if (token == L"up") return VK_UP;
    if (token == L"down") return VK_DOWN;
    if (token.rfind(L"f", 0) == 0 && token.size() <= 3) {
        int n = _wtoi(token.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }
    return 0;
}

Hotkey ParseHotkey(const std::wstring& text) {
    Hotkey hk;
    UINT mods = 0;
    UINT vk = 0;
    auto addMod = [&](UINT mod, int idx, UINT sideVk) {
        mods |= mod;
        if (sideVk) hk.modVk[idx] = sideVk;
    };
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find(L'+', start);
        std::wstring part = Trim(text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
        std::wstring low = ToLower(part);
        if (!low.empty()) {
            if (low == L"ctrl" || low == L"control") {
                addMod(MOD_CONTROL, 0, 0);
            } else if (low == L"lctrl" || low == L"leftctrl" || low == L"leftcontrol") {
                addMod(MOD_CONTROL, 0, VK_LCONTROL);
            } else if (low == L"rctrl" || low == L"rightctrl" || low == L"rightcontrol") {
                addMod(MOD_CONTROL, 0, VK_RCONTROL);
            } else if (low == L"alt" || low == L"menu") {
                addMod(MOD_ALT, 1, 0);
            } else if (low == L"lalt" || low == L"leftalt") {
                addMod(MOD_ALT, 1, VK_LMENU);
            } else if (low == L"ralt" || low == L"rightalt") {
                addMod(MOD_ALT, 1, VK_RMENU);
            } else if (low == L"shift") {
                addMod(MOD_SHIFT, 2, 0);
            } else if (low == L"lshift" || low == L"leftshift") {
                addMod(MOD_SHIFT, 2, VK_LSHIFT);
            } else if (low == L"rshift" || low == L"rightshift") {
                addMod(MOD_SHIFT, 2, VK_RSHIFT);
            } else if (low == L"win" || low == L"windows" || low == L"meta") {
                addMod(MOD_WIN, 3, 0);
            } else if (low == L"lwin" || low == L"leftwin") {
                addMod(MOD_WIN, 3, VK_LWIN);
            } else if (low == L"rwin" || low == L"rightwin") {
                addMod(MOD_WIN, 3, VK_RWIN);
            } else {
                vk = ParseVkToken(part);
            }
        }
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }
    hk.modifiers = mods;
    hk.vk = vk;
    hk.modifierOnly = (vk == 0 && mods != 0);
    hk.valid = (vk != 0) || hk.modifierOnly;
    if (vk != 0) {
        // Side-specific info is only meaningful for modifier-only shortcuts.
        for (auto& m : hk.modVk) m = 0;
    }
    return hk;
}

void LoadSettings() {
    Settings s;
    PCWSTR hotkeyStr = Wh_GetStringSetting(L"hotkey");
    if (hotkeyStr && *hotkeyStr) s.hotkeyStr = hotkeyStr;
    if (hotkeyStr) Wh_FreeStringSetting(hotkeyStr);

    PCWSTR mode = Wh_GetStringSetting(L"mode");
    if (mode && *mode) s.mode = mode;
    if (mode) Wh_FreeStringSetting(mode);

    PCWSTR scope = Wh_GetStringSetting(L"scope");
    if (scope && *scope) s.scope = scope;
    if (scope) Wh_FreeStringSetting(scope);

    s.restoreClipboard = Wh_GetIntSetting(L"restoreClipboard") != 0;
    s.switchLayout = Wh_GetIntSetting(L"switchLayout") != 0;
    s.showNotification = Wh_GetIntSetting(L"showNotification") != 0;
    s.actionDelayMs = Wh_GetIntSetting(L"actionDelayMs");
    if (s.actionDelayMs <= 0) s.actionDelayMs = 40;
    if (s.actionDelayMs < 10) s.actionDelayMs = 10;
    if (s.actionDelayMs > 500) s.actionDelayMs = 500;

    s.hotkey = ParseHotkey(s.hotkeyStr);

    Logf(L"Settings: hotkey=%s valid=%d mode=%s scope=%s restoreClipboard=%d switchLayout=%d showNotification=%d delay=%d",
         s.hotkeyStr.c_str(), s.hotkey.valid ? 1 : 0, s.mode.c_str(), s.scope.c_str(),
         s.restoreClipboard ? 1 : 0, s.switchLayout ? 1 : 0, s.showNotification ? 1 : 0,
         s.actionDelayMs);

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings = s;
}

Settings GetSettingsCopy() {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

bool IsExtendedVk(UINT vk) {
    switch (vk) {
        case VK_RMENU: case VK_RCONTROL: case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_NUMLOCK: case VK_CANCEL: case VK_SNAPSHOT: case VK_DIVIDE:
        case VK_LWIN: case VK_RWIN: case VK_APPS:
            return true;
        default:
            return false;
    }
}

void SendKeyCombo(UINT vk, bool ctrl, bool shift, bool alt, bool win) {
    std::vector<INPUT> inputs;
    auto add = [&](WORD k, bool down, bool extended = false) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = k;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        if (extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        inputs.push_back(in);
    };
    if (ctrl) add(VK_CONTROL, true);
    if (shift) add(VK_SHIFT, true);
    if (alt) add(VK_MENU, true);
    if (win) add(VK_LWIN, true, true);
    add((WORD)vk, true, IsExtendedVk(vk));
    add((WORD)vk, false, IsExtendedVk(vk));
    if (win) add(VK_LWIN, false, true);
    if (alt) add(VK_MENU, false);
    if (shift) add(VK_SHIFT, false);
    if (ctrl) add(VK_CONTROL, false);
    if (!inputs.empty()) SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

bool WaitForClipboardChange(DWORD seqBefore, DWORD timeoutMs) {
    ULONGLONG start = GetTickCount64();
    for (;;) {
        if (GetClipboardSequenceNumber() != seqBefore) return true;
        if (GetTickCount64() - start >= timeoutMs) return false;
        Sleep(5);
    }
}

void RequestConversion() {
    if (g_convertThreadId) {
        PostThreadMessage(g_convertThreadId, WM_APP_CONVERT, 0, 0);
    }
}

bool AnyModifierDown() {
    return g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL] || g_keyDown[VK_CONTROL] ||
           g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU] || g_keyDown[VK_MENU] ||
           g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT] || g_keyDown[VK_SHIFT] ||
           g_keyDown[VK_LWIN] || g_keyDown[VK_RWIN];
}

bool HotkeyModifiersMatch(const Hotkey& hk) {
    bool ctrl = g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL] || g_keyDown[VK_CONTROL];
    bool alt = g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU] || g_keyDown[VK_MENU];
    bool shift = g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT] || g_keyDown[VK_SHIFT];
    bool win = g_keyDown[VK_LWIN] || g_keyDown[VK_RWIN];
    return ctrl == ((hk.modifiers & MOD_CONTROL) != 0) &&
           alt == ((hk.modifiers & MOD_ALT) != 0) &&
           shift == ((hk.modifiers & MOD_SHIFT) != 0) &&
           win == ((hk.modifiers & MOD_WIN) != 0);
}

bool IsModifierVk(UINT vk) {
    switch (vk) {
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
        case VK_LMENU: case VK_RMENU: case VK_MENU:
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
        case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

// Modifier-only shortcut (e.g. LCtrl+LShift): exactly the configured modifier
// classes must be held, on the configured side when a side was specified.
bool ModifierOnlyHotkeyMatch(const Hotkey& hk) {
    bool ctrl = g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL];
    bool alt = g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU];
    bool shift = g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT];
    bool win = g_keyDown[VK_LWIN] || g_keyDown[VK_RWIN];
    if (ctrl != ((hk.modifiers & MOD_CONTROL) != 0) ||
        alt != ((hk.modifiers & MOD_ALT) != 0) ||
        shift != ((hk.modifiers & MOD_SHIFT) != 0) ||
        win != ((hk.modifiers & MOD_WIN) != 0)) {
        return false;
    }
    for (UINT sideVk : hk.modVk) {
        if (sideVk && !g_keyDown[sideVk]) return false;
    }
    return true;
}

// True once every key of the combo (and any stray modifier) has been released.
bool HotkeyComboReleased(const Hotkey& hk) {
    if (hk.vk != 0 && hk.vk < 256 && g_keyDown[hk.vk]) return false;
    return !AnyModifierDown();
}

bool OpenClipboardWithRetry(HWND owner = nullptr) {
    ULONGLONG start = GetTickCount64();
    for (;;) {
        if (OpenClipboard(owner)) return true;
        if (GetTickCount64() - start >= 100) return false;
        Sleep(2);
    }
}

std::wstring GetClipboardUnicodeText() {
    std::wstring text;
    if (!OpenClipboardWithRetry()) return text;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            text = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return text;
}

bool SetClipboardUnicodeText(const std::wstring& text) {
    if (!OpenClipboardWithRetry()) return false;
    EmptyClipboard();
    SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
        CloseClipboard();
        return false;
    }
    void* p = GlobalLock(h);
    if (p) {
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(h);
    }
    bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(h);
    return ok;
}

void BuildLayoutMap(HKL hkl, LayoutMap& map) {
    map = {};
    map.hkl = hkl;
    for (int shift = 0; shift <= 1; shift++) {
        BYTE state[256] = {};
        if (shift) state[VK_SHIFT] = 0x80;
        for (UINT vk = 0x20; vk < 256; vk++) {
            if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS ||
                vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL ||
                vk == VK_PROCESSKEY || vk == VK_SNAPSHOT) {
                continue;
            }
            // Focus on keys likely to produce characters.
            if ((vk > 0x5A && vk < 0x60) || (vk > 0x6F && vk < 0xBA) || vk > 0xC0 || (vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) || vk == VK_SPACE || (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || (vk >= VK_OEM_102 && vk <= VK_OEM_102)) {
                // allowed range candidates
            } else {
                continue;
            }

            UINT scan = MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, hkl);
            WCHAR buf[8] = {};
            BYTE localState[256];
            memcpy(localState, state, sizeof(localState));
            int rc = ToUnicodeEx(vk, scan, localState, buf, 8, 0, hkl);
            if (rc == -1) {
                // Flush pending dead-key state on the local state, not a shared one.
                WCHAR dummy[8] = {};
                ToUnicodeEx(VK_SPACE, MapVirtualKeyEx(VK_SPACE, MAPVK_VK_TO_VSC_EX, hkl), localState, dummy, 8, 0, hkl);
                continue;
            }
            if (rc > 0) {
                wchar_t ch = buf[0];
                map.charsForKey[vk][shift] = std::wstring(1, ch);
                auto& rev = shift ? map.keyForCharShift : map.keyForChar;
                if (rev.find(ch) == rev.end()) rev[ch] = KeyStroke{vk, scan, shift != 0};
                if (iswalpha(ch)) {
                    wchar_t lo = (wchar_t)towlower(ch);
                    wchar_t up = (wchar_t)towupper(ch);
                    if (rev.find(lo) == rev.end()) rev[lo] = KeyStroke{vk, scan, shift != 0};
                    if (rev.find(up) == rev.end()) rev[up] = KeyStroke{vk, scan, shift != 0};
                }
            }
        }
    }
}

// Caller must hold g_layoutMutex.
void EnsureLayoutMapsBuiltLocked() {
    for (HKL hkl : g_layouts) {
        UINT_PTR key = (UINT_PTR)hkl;
        if (g_layoutMaps.find(key) == g_layoutMaps.end()) {
            LayoutMap map;
            BuildLayoutMap(hkl, map);
            g_layoutMaps.emplace(key, std::move(map));
        }
    }
}

bool RefreshLayoutsIfNeeded(bool forceRebuild = false) {
    std::lock_guard<std::mutex> lock(g_layoutMutex);
    int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0) return false;
    std::vector<HKL> layouts(count);
    count = GetKeyboardLayoutList(count, layouts.data());
    if (count <= 0) return false;
    layouts.resize(count);

    bool changed = forceRebuild || (layouts.size() != g_layouts.size());
    if (!changed) {
        for (size_t i = 0; i < layouts.size(); i++) {
            if (layouts[i] != g_layouts[i]) { changed = true; break; }
        }
    }
    if (changed) {
        g_layouts = std::move(layouts);
        g_layoutMaps.clear();
    }
    if (g_layouts.empty()) return false;
    EnsureLayoutMapsBuiltLocked();
    return true;
}

// Caller must hold g_layoutMutex.
LayoutMap& GetOrBuildLayoutMapLocked(HKL hkl) {
    UINT_PTR key = (UINT_PTR)hkl;
    auto it = g_layoutMaps.find(key);
    if (it != g_layoutMaps.end()) return it->second;
    LayoutMap map;
    BuildLayoutMap(hkl, map);
    auto [insIt, _] = g_layoutMaps.emplace(key, std::move(map));
    return insIt->second;
}

HKL GetForegroundKeyboardLayout() {
    HWND fg = GetForegroundWindow();
    if (!fg) return GetKeyboardLayout(0);
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    if (!tid) return GetKeyboardLayout(0);
    HKL hkl = GetKeyboardLayout(tid);
    return hkl ? hkl : GetKeyboardLayout(0);
}

bool IsLetterLike(wchar_t c) { return iswalpha((wint_t)c) != 0; }

int ScriptBucket(wchar_t c) {
    if ((c >= 0x0041 && c <= 0x024F) || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return 1; // Latin-ish
    if (c >= 0x0400 && c <= 0x052F) return 2; // Cyrillic
    if (c >= 0x0590 && c <= 0x05FF) return 3; // Hebrew
    if (c >= 0x0600 && c <= 0x06FF) return 4; // Arabic
    if (c >= 0x0370 && c <= 0x03FF) return 5; // Greek
    return 0;
}

int LayoutScriptHint(HKL hkl) {
    LANGID lang = LOWORD((UINT_PTR)hkl);
    WORD primary = PRIMARYLANGID(lang);
    switch (primary) {
        case LANG_RUSSIAN: case LANG_UKRAINIAN: case LANG_BELARUSIAN: case LANG_BULGARIAN:
            return 2;
        case LANG_HEBREW:
            return 3;
        case LANG_ARABIC: case LANG_PERSIAN: case LANG_URDU:
            return 4;
        case LANG_GREEK:
            return 5;
        default:
            return 1; // default Latin bucket
    }
}

// Caller must hold g_layoutMutex.
std::wstring ConvertUsingPair(const std::wstring& text, HKL source, HKL target, int& score) {
    LayoutMap& src = GetOrBuildLayoutMapLocked(source);
    LayoutMap& dst = GetOrBuildLayoutMapLocked(target);
    std::wstring out;
    out.reserve(text.size());
    score = 0;
    int convertible = 0;
    int targetScript = LayoutScriptHint(target);
    for (wchar_t c : text) {
        const KeyStroke* ksPtr = nullptr;
        auto it = src.keyForChar.find(c);
        if (it != src.keyForChar.end()) ksPtr = &it->second;
        if (!ksPtr) {
            auto it2 = src.keyForCharShift.find(c);
            if (it2 != src.keyForCharShift.end()) ksPtr = &it2->second;
        }
        if (ksPtr) {
            const KeyStroke& ks = *ksPtr;
            const std::wstring& mapped = dst.charsForKey[ks.vk][ks.shift ? 1 : 0];
            if (!mapped.empty()) {
                wchar_t outCh = mapped[0];
                // Preserve basic letter case when possible.
                if (IsLetterLike(c) && IsLetterLike(outCh)) {
                    if (iswupper(c)) outCh = (wchar_t)towupper(outCh);
                    else if (iswlower(c)) outCh = (wchar_t)towlower(outCh);
                }
                out += outCh;
                convertible++;
                score += 2;
                int sb = ScriptBucket(outCh);
                if (sb == 0 || sb == targetScript) score += 1;
                continue;
            }
        }
        out += c;
        if (!IsLetterLike(c)) score += 1;
    }
    // Prefer conversions that actually change a meaningful portion of convertible text.
    if (out != text) score += convertible * 2;
    else score -= 5;
    return out;
}

bool ActivateTargetKeyboardLayout(HKL hkl) {
    if (!hkl) return false;
    HWND fg = GetForegroundWindow();
    if (fg) {
        PostMessage(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)hkl);
    }
    return ::ActivateKeyboardLayout(hkl, KLF_SETFORPROCESS) != nullptr;
}

bool ConvertTextAutoOrCycle(const std::wstring& input, const Settings& s, std::wstring& output, HKL& chosenTarget) {
    output.clear();
    chosenTarget = nullptr;
    std::lock_guard<std::mutex> lock(g_layoutMutex);
    if (input.empty() || g_layouts.size() < 2) return false;

    // Ensure maps exist (cheap if cached).
    for (HKL hkl : g_layouts) GetOrBuildLayoutMapLocked(hkl);

    HKL active = GetForegroundKeyboardLayout();

    if (s.mode == L"cycle") {
        // Pick a source layout that best "owns" the input characters, then cycle target.
        HKL src = active ? active : g_layouts[0];
        int bestSrcScore = -1000000;
        for (HKL cand : g_layouts) {
            LayoutMap& m = GetOrBuildLayoutMapLocked(cand);
            int sc = 0;
            for (wchar_t c : input) {
                if (m.keyForChar.find(c) != m.keyForChar.end() || m.keyForCharShift.find(c) != m.keyForCharShift.end()) sc += 2;
                else if (!IsLetterLike(c)) sc += 1;
            }
            if (cand == active) sc += 1;
            if (sc > bestSrcScore) { bestSrcScore = sc; src = cand; }
        }
        size_t start = 0;
        for (size_t i = 0; i < g_layouts.size(); i++) if (g_layouts[i] == src) { start = i; break; }
        size_t idx = (start + 1 + (g_cycleIndex % (g_layouts.size() - 1))) % g_layouts.size();
        if (idx == start) idx = (idx + 1) % g_layouts.size();
        g_cycleIndex++;
        HKL target = g_layouts[idx];
        int score = 0;
        output = ConvertUsingPair(input, src, target, score);
        chosenTarget = target;
        return output != input;
    }

    int bestScore = -1000000;
    std::wstring bestOut;
    HKL bestTarget = nullptr;

    auto scriptMatchScore = [&](HKL src, HKL dst, const std::wstring& outText) {
        int s = 0;
        int srcHint = LayoutScriptHint(src);
        int dstHint = LayoutScriptHint(dst);
        for (wchar_t c : input) {
            int sb = ScriptBucket(c);
            if (sb == 0) continue;
            if (sb == srcHint) s += 1;
            if (sb == dstHint) s -= 1;
        }
        for (wchar_t c : outText) {
            int sb = ScriptBucket(c);
            if (sb == 0) continue;
            if (sb == dstHint) s += 2;
            if (sb == srcHint) s -= 1;
        }
        if (src == active) s += 1;
        return s;
    };

    for (HKL src : g_layouts) {
        for (HKL dst : g_layouts) {
            if (src == dst) continue;
            int score = 0;
            std::wstring out = ConvertUsingPair(input, src, dst, score);
            if (out.empty() || out == input) continue;
            score += scriptMatchScore(src, dst, out);
            if (score > bestScore) {
                bestScore = score;
                bestOut = std::move(out);
                bestTarget = dst;
            }
        }
    }

    if (bestOut.empty() || bestOut == input) return false;
    output = std::move(bestOut);
    chosenTarget = bestTarget;
    return true;
}

// Safety net: conversion is normally requested on combo release already.
void WaitForModifierKeysRelease(DWORD timeoutMs = 250) {
    DWORD start = GetTickCount();
    for (;;) {
        bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
                    (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                    (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                    (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                    (GetAsyncKeyState(VK_RWIN) & 0x8000);
        if (!down) return;
        if (GetTickCount() - start > timeoutMs) return;
        Sleep(10);
    }
}

constexpr const wchar_t* TOAST_MESSAGE = L"Text converted";
constexpr UINT_PTR TOAST_TIMER_ID = 1;
constexpr UINT TOAST_TIMER_MS = 16;
constexpr ULONGLONG TOAST_HOLD_MS = 1100;
constexpr ULONGLONG TOAST_FADE_OUT_MS = 240;
constexpr BYTE TOAST_MAX_ALPHA = 235;

LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int w = rc.right;
            const int h = rc.bottom;
            const int saved = SaveDC(dc);

            // Card background with a subtle border (shadcn-like dark toast).
            HBRUSH bg = CreateSolidBrush(RGB(24, 24, 27));        // zinc-900
            HPEN border = CreatePen(PS_SOLID, 1, RGB(63, 63, 70)); // zinc-700
            SelectObject(dc, bg);
            SelectObject(dc, border);
            RoundRect(dc, 0, 0, w, h, 14, 14);

            // Success badge: green circle with a white check mark.
            const int cx = 24;
            const int cy = h / 2;
            const int r = 9;
            HBRUSH badge = CreateSolidBrush(RGB(34, 197, 94)); // green-500
            HPEN badgePen = CreatePen(PS_SOLID, 1, RGB(34, 197, 94));
            SelectObject(dc, badge);
            SelectObject(dc, badgePen);
            Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);

            HPEN checkPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            SelectObject(dc, checkPen);
            POINT check[3] = {
                {cx - 4, cy},
                {cx - 1, cy + 3},
                {cx + 5, cy - 4},
            };
            Polyline(dc, check, 3);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(250, 250, 250)); // zinc-50
            SelectObject(dc, g_toastFont ? g_toastFont : GetStockObject(DEFAULT_GUI_FONT));
            RECT textRc = {cx + r + 10, 0, w - 16, h};
            DrawTextW(dc, TOAST_MESSAGE, -1, &textRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RestoreDC(dc, saved);
            DeleteObject(checkPen);
            DeleteObject(badgePen);
            DeleteObject(badge);
            DeleteObject(border);
            DeleteObject(bg);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == TOAST_TIMER_ID) {
                ULONGLONG elapsed = GetTickCount64() - g_toastStartTick;
                ULONGLONG end = TOAST_HOLD_MS + TOAST_FADE_OUT_MS;
                if (elapsed < TOAST_HOLD_MS) {
                    SetLayeredWindowAttributes(hwnd, 0, TOAST_MAX_ALPHA, LWA_ALPHA);
                } else if (elapsed < end) {
                    BYTE alpha = (BYTE)(TOAST_MAX_ALPHA * (end - elapsed) / TOAST_FADE_OUT_MS);
                    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
                } else {
                    KillTimer(hwnd, TOAST_TIMER_ID);
                    DestroyWindow(hwnd);
                    return 0;
                }
            }
            return 0;
        case WM_DESTROY:
            if (g_toastFont) {
                DeleteObject(g_toastFont);
                g_toastFont = nullptr;
            }
            if (g_toastHwnd == hwnd) {
                g_toastHwnd = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Fixed corner success toast, independent of the active window.
void ShowToast() {
    if (g_toastHwnd) {
        DestroyWindow(g_toastHwnd);
        g_toastHwnd = nullptr;
    }

    if (!g_toastClassRegistered) {
        HINSTANCE inst = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ToastWndProc), &inst);
        if (!inst) {
            inst = GetModuleHandleW(nullptr);
        }
        g_toastInstance = inst;

        // Unique per load, so reloading the mod can never collide with a stale class.
        _snwprintf_s(g_toastClass, _countof(g_toastClass), _TRUNCATE,
                     L"WindhawkKLTC_Toast_%llx", (unsigned long long)GetTickCount64());

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ToastWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = g_toastClass;
        if (!RegisterClassExW(&wc)) {
            Logf(L"RegisterClassEx(toast) failed: %u", GetLastError());
            return;
        }
        g_toastClassRegistered = true;
        Logf(L"Toast class registered: %s", g_toastClass);
    }

    if (g_toastFont) {
        DeleteObject(g_toastFont);
        g_toastFont = nullptr;
    }
    g_toastFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const int textLeft = 24 + 9 + 10;
    const int h = 46;
    int w = 220;
    {
        HDC dc = GetDC(nullptr);
        HGDIOBJ oldFont = SelectObject(dc, g_toastFont);
        SIZE sz = {};
        if (GetTextExtentPoint32W(dc, TOAST_MESSAGE, (int)wcslen(TOAST_MESSAGE), &sz)) {
            w = textLeft + (int)sz.cx + 18;
        }
        SelectObject(dc, oldFont);
        ReleaseDC(nullptr, dc);
    }

    // Bottom-right corner of the monitor under the cursor.
    POINT cursor = {};
    GetCursorPos(&cursor);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);

    const int margin = 24;
    int x = mi.rcWork.right - w - margin;
    int y = mi.rcWork.bottom - h - margin;
    if (x < mi.rcWork.left + margin) x = mi.rcWork.left + margin;
    if (y < mi.rcWork.top + margin) y = mi.rcWork.top + margin;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        g_toastClass, L"", WS_POPUP, x, y, w, h, nullptr, nullptr,
        g_toastInstance, nullptr);
    if (!hwnd) {
        Logf(L"CreateWindowEx(toast) failed: %u", GetLastError());
        return;
    }
    g_toastHwnd = hwnd;
    SetLayeredWindowAttributes(hwnd, 0, TOAST_MAX_ALPHA, LWA_ALPHA);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 14, 14);
    if (rgn && !SetWindowRgn(hwnd, rgn, TRUE)) {
        DeleteObject(rgn);
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    g_toastStartTick = GetTickCount64();
    SetTimer(hwnd, TOAST_TIMER_ID, TOAST_TIMER_MS, nullptr);
    Logf(L"Toast shown at %d,%d (%dx%d)", x, y, w, h);
}

// Backstop so the toast can never get stuck if timer messages are missed.
void ExpireToastIfNeeded() {
    if (g_toastHwnd &&
        GetTickCount64() - g_toastStartTick >= TOAST_HOLD_MS + TOAST_FADE_OUT_MS) {
        DestroyWindow(g_toastHwnd);
        g_toastHwnd = nullptr;
    }
}

void PerformConversion() {
    std::lock_guard<std::mutex> opLock(g_convertMutex);
    ULONGLONG tStart = GetTickCount64();
    Logf(L"PerformConversion start");
    Settings s = GetSettingsCopy();
    if (!s.hotkey.valid) {
        Logf(L"Hotkey is not valid: %s", s.hotkeyStr.c_str());
        return;
    }

    // Normally a no-op: conversion is requested once the combo is released.
    WaitForModifierKeysRelease();
    ULONGLONG tWaitDone = GetTickCount64();

    // Pick up layouts added/removed at runtime; cheap when unchanged.
    RefreshLayoutsIfNeeded();

    std::wstring originalClipboard;
    if (s.restoreClipboard) originalClipboard = GetClipboardUnicodeText();

    std::wstring selected;
    bool captured = false;
    bool usedFallbackWord = false;
    const DWORD copyTimeoutMs = (std::max)(150u, (unsigned)s.actionDelayMs * 3u);

    if (s.scope == L"selection-first" || s.scope == L"selection-only") {
        DWORD seq = GetClipboardSequenceNumber();
        SendKeyCombo('C', true, false, false, false); // Ctrl+C
        if (WaitForClipboardChange(seq, copyTimeoutMs)) {
            selected = GetClipboardUnicodeText();
            captured = !selected.empty();
        }
    }

    if (!captured && s.scope != L"selection-only") {
        DWORD seq = GetClipboardSequenceNumber();
        SendKeyCombo(VK_LEFT, true, true, false, false); // Ctrl+Shift+Left
        SendKeyCombo('C', true, false, false, false);    // Ctrl+C
        if (WaitForClipboardChange(seq, copyTimeoutMs)) {
            selected = GetClipboardUnicodeText();
            usedFallbackWord = !selected.empty();
        }
    }

    ULONGLONG tCopyDone = GetTickCount64();

    if (selected.empty()) {
        Logf(L"No text captured for conversion (wait=%llums copy=%llums)",
             tWaitDone - tStart, tCopyDone - tWaitDone);
        if (s.restoreClipboard) SetClipboardUnicodeText(originalClipboard);
        return;
    }

    std::wstring converted;
    HKL targetLayout = nullptr;
    bool convertedOk = ConvertTextAutoOrCycle(selected, s, converted, targetLayout);
    ULONGLONG tConvertDone = GetTickCount64();
    if (!convertedOk || converted.empty() || converted == selected) {
        Logf(L"Conversion produced no change (len=%zu)", selected.size());
        if (usedFallbackWord) {
            SendKeyCombo(VK_RIGHT, false, false, false, false);
        }
        if (s.restoreClipboard) SetClipboardUnicodeText(originalClipboard);
        return;
    }

    if (!SetClipboardUnicodeText(converted)) {
        Logf(L"Failed to set converted clipboard text");
        if (s.restoreClipboard) SetClipboardUnicodeText(originalClipboard);
        return;
    }

    SendKeyCombo('V', true, false, false, false); // Ctrl+V
    ULONGLONG tPasteDone = GetTickCount64();

    if (s.showNotification) {
        ShowToast();
    }

    Sleep(s.restoreClipboard ? 150 : 40);

    if (s.switchLayout && targetLayout) {
        ActivateTargetKeyboardLayout(targetLayout);
    }

    if (s.restoreClipboard) {
        SetClipboardUnicodeText(originalClipboard);
    }
    ULONGLONG tEnd = GetTickCount64();
    Logf(L"Conversion applied (len %zu -> %zu) wait=%llums copy=%llums convert=%llums paste=%llums total=%llums",
         selected.size(), converted.size(),
         tWaitDone - tStart, tCopyDone - tWaitDone, tConvertDone - tCopyDone,
         tPasteDone - tConvertDone, tEnd - tStart);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;
        if (kb && kb->vkCode < 256 && !(kb->flags & LLKHF_INJECTED)) {
            const UINT vk = kb->vkCode;
            Settings s = GetSettingsCopy();
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                const bool wasDown = g_keyDown[vk];
                g_keyDown[vk] = true;
                if (s.hotkey.valid) {
                    if (s.hotkey.modifierOnly) {
                        if (!g_hotkeyPending) {
                            if (ModifierOnlyHotkeyMatch(s.hotkey)) {
                                g_hotkeyPending = true;
                            }
                        } else if (!IsModifierVk(vk) || !ModifierOnlyHotkeyMatch(s.hotkey)) {
                            // Another key or an extra modifier went down: this is a
                            // regular shortcut such as Ctrl+Shift+T, not a bare combo.
                            g_hotkeyPending = false;
                        }
                    } else if (vk == s.hotkey.vk) {
                        if (!wasDown && !g_hotkeyPending && HotkeyModifiersMatch(s.hotkey)) {
                            Logf(L"Hotkey matched, waiting for release before conversion");
                            g_suppressedHotkeyVk = vk;
                            g_hotkeyPending = true;
                            return 1; // suppress hotkey keydown
                        }
                        if (g_hotkeyPending || g_suppressedHotkeyVk == vk) {
                            return 1; // swallow auto-repeat while the combo is held
                        }
                    } else if (g_hotkeyPending && !IsModifierVk(vk)) {
                        g_hotkeyPending = false;
                    }
                }
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                g_keyDown[vk] = false;
                const bool suppress = (g_suppressedHotkeyVk != 0 && vk == g_suppressedHotkeyVk);
                if (suppress) {
                    g_suppressedHotkeyVk = 0;
                }
                if (g_hotkeyPending && s.hotkey.valid && HotkeyComboReleased(s.hotkey)) {
                    g_hotkeyPending = false;
                    Logf(L"Hotkey released, requesting conversion");
                    RequestConversion();
                }
                if (suppress) {
                    return 1; // suppress matching keyup to avoid stray character/input
                }
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

DWORD WINAPI WorkerThreadProc(LPVOID) {
    g_workerThreadId = GetCurrentThreadId();
    MSG msg;
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
    if (!g_keyboardHook) {
        Logf(L"SetWindowsHookEx(WH_KEYBOARD_LL) failed: %u", GetLastError());
    } else {
        Logf(L"Keyboard hook installed (thread=%lu)", GetCurrentThreadId());
    }

    // Pending hotkey state is cleared by key release or by any other key press;
    // no timer is needed (and a timeout must not fire with modifiers still held).

    if (!RefreshLayoutsIfNeeded(true)) {
        Logf(L"No keyboard layouts found or failed to enumerate layouts");
    } else {
        Logf(L"Keyboard layouts enumerated and maps built");
    }

    for (;;) {
        DWORD wait = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                goto done;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

done:
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    g_workerThreadId = 0;
    return 0;
}

DWORD WINAPI ConversionThreadProc(LPVOID) {
    MSG msg;
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_convertThreadId = GetCurrentThreadId();
    Logf(L"Conversion thread started (thread=%lu)", GetCurrentThreadId());

    for (;;) {
        DWORD wait = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE,
                                               g_toastHwnd ? 100 : INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait == WAIT_TIMEOUT) {
            ExpireToastIfNeeded();
            continue;
        }
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                goto done;
            }
            if (msg.message == WM_APP_CONVERT) {
                PerformConversion();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

done:
    if (g_toastHwnd) {
        DestroyWindow(g_toastHwnd);
        g_toastHwnd = nullptr;
    }
    g_convertThreadId = 0;
    return 0;
}

}  // namespace

BOOL WhTool_ModInit() {
    Logf(L"Tool mod init (pid=%lu)", GetCurrentProcessId());
    LoadSettings();

    g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        Logf(L"CreateEvent failed: %u", GetLastError());
        return FALSE;
    }

    g_convertThread = CreateThread(nullptr, 0, ConversionThreadProc, nullptr, 0, nullptr);
    if (!g_convertThread) {
        Logf(L"CreateThread (conversion) failed: %u", GetLastError());
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return FALSE;
    }

    g_workerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
    if (!g_workerThread) {
        Logf(L"CreateThread (hook) failed: %u", GetLastError());
        SetEvent(g_stopEvent);
        WaitForSingleObject(g_convertThread, 5000);
        CloseHandle(g_convertThread);
        g_convertThread = nullptr;
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return FALSE;
    }

    return TRUE;
}

void WhTool_ModSettingsChanged() {
    Logf(L"Settings changed");
    LoadSettings();
    RefreshLayoutsIfNeeded(true);
}

void WhTool_ModUninit() {
    Logf(L"Uninit");
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    if (g_workerThread) {
        // Nudge message loop
        if (g_workerThreadId) PostThreadMessage(g_workerThreadId, WM_NULL, 0, 0);
        WaitForSingleObject(g_workerThread, 5000);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
    if (g_convertThread) {
        if (g_convertThreadId) PostThreadMessage(g_convertThreadId, WM_NULL, 0, 0);
        WaitForSingleObject(g_convertThread, 5000);
        CloseHandle(g_convertThread);
        g_convertThread = nullptr;
    }
    if (g_toastClassRegistered && g_toastClass[0]) {
        UnregisterClassW(g_toastClass, g_toastInstance);
        g_toastClassRegistered = false;
        g_toastClass[0] = L'\0';
        g_toastInstance = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    memset(g_keyDown, 0, sizeof(g_keyDown));
    g_hotkeyPending = false;
    g_suppressedHotkeyVk = 0;
    g_layoutMaps.clear();
    g_layouts.clear();
}

BOOL Wh_ModInit() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    Logf(L"Wh_ModInit pid=%lu session=%lu cmd=\"%s\"",
         GetCurrentProcessId(), sessionId, GetCommandLineW() ? GetCommandLineW() : L"");

    // Run in explorer.exe as a simple in-process tool.
    return WhTool_ModInit() ? TRUE : FALSE;
}

void Wh_ModAfterInit() {}

void Wh_ModSettingsChanged() {
    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    WhTool_ModUninit();
}

