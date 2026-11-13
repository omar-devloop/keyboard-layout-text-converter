# Keyboard Layout Text Converter (Windhawk mod)

A [Windhawk](https://windhawk.net/) mod that converts text typed with the wrong keyboard layout in place, across most Windows applications.

Select the mistyped text and press **Alt+X**. The mod copies the selection, remaps it by key positions between the keyboard layouts installed on your device, and pastes the converted text back.

## Features

- Converts a selection with a global shortcut, with an optional previous-word fallback.
- Detects the most plausible source/target layout among the installed layouts, or cycles through them.
- Supports Latin, Cyrillic, Hebrew, Arabic, and Greek layouts for conversion scoring.
- Optional full clipboard restore (all formats, up to 128 MB per format) and optional keyboard-layout switch after conversion.
- Optional small success toast in the screen corner (off by default).
- Runs as a Windhawk tool mod in a dedicated `windhawk.exe` process.

## Install

1. Install [Windhawk](https://windhawk.net/).
2. In Windhawk, create a new mod from source and paste the contents of [`mods/keyboard-layout-text-converter.wh.cpp`](mods/keyboard-layout-text-converter.wh.cpp).
3. Compile and enable the mod.

## Usage

- Select text and press the conversion shortcut (default `Alt+X`).
- With the default *selection-first* scope, pressing the shortcut without a selection converts the previous word.
- Regular combinations such as `Ctrl+Alt+Space` are supported. The shortcut setting accepts `L`/`R` prefixes to pick a specific side (e.g. `LCtrl+LShift`, `RCtrl+F12`).

## Settings

- **Conversion shortcut** — global shortcut; a regular combination such as `Alt+X` is recommended, since modifier-only combinations can collide with Windows actions (for example `Ctrl+Shift` switches the keyboard layout).
- **Conversion mode** — auto-detect layouts or cycle through installed layouts.
- **Capture scope** — selection with previous-word fallback, selection only, or previous word only.
- **Restore clipboard** — restore the previous clipboard content, in all of its formats, after conversion. Formats above 128 MB (or 256 MB in total) can't be preserved, and the conversion is skipped in that case.
- **Show notification** - show a small success toast in the screen corner after conversion (off by default).
- **Switch keyboard layout after convert** — switch to the detected target layout.
- **Clipboard wait (ms)** — time to wait for the clipboard after copy and to wait after pasting before restoring it; values below 150 are raised to 150. Raise it for slow applications.

## Notes and limitations

- Replacement is clipboard-based (simulated copy/paste), so behavior can vary in apps with custom clipboard/shortcut handling.
- The mod may not interact with elevated (UIPI) windows, secure fields, or some terminals/remote apps.
- Modifier-only shortcuts only fire when the keys are pressed and released on their own. Pressing another key (e.g. `Ctrl+Shift+T`) cancels the shortcut, so normal `Ctrl+Shift+...` shortcuts keep working. They can still collide with Windows actions such as switching the keyboard layout, which is why a regular combination is the default.
- In console windows (conhost, Windows Terminal and mintty) the copy uses `Ctrl+Insert` instead of `Ctrl+C`, because `Ctrl+C` without a selection discards the typed line and interrupts a running command. The previous-word fallback is skipped there, and the converted text is left on the clipboard instead of being pasted — a terminal selection covers the screen buffer, not the input line, so pasting would add a duplicate. Delete the original and paste manually; the previous clipboard content isn't restored in that case. Only terminals whose `Ctrl+Insert` copies to the Windows clipboard are handled this way; elsewhere (ConEmu/Cmder, WezTerm, Alacritty, PuTTY, VS Code's integrated terminal, …) the mod sends `Ctrl+C`, which may interrupt the running program.
- If the clipboard holds content that can't be preserved (for example a single format above 128 MB), the mod beeps and does not convert, so nothing is lost.
- Editors such as VS Code or Visual Studio copy the current line when nothing is selected, so pressing the shortcut without a selection can paste an extra line there; the *selection-only* scope doesn't avoid this.
- Very short or ambiguous text can be hard to auto-detect; use a longer selection or adjust settings if needed.

## Similar mods

- [Arabic Keyboard Layout Fixer](https://windhawk.net/mods/arabic-keyboard-layout-fixer) converts between a fixed Arabic/English pair.
- [Quick translator & Layout-switcher](https://windhawk.net/mods/quick-translator-layout-switcher) converts between a fixed English/Russian pair, with optional online translation.

Both use a fixed layout pair. This mod builds the mapping from every installed layout, and adds a previous-word fallback, cycle mode, optional layout switching and a toast.

## Source

[`mods/keyboard-layout-text-converter.wh.cpp`](mods/keyboard-layout-text-converter.wh.cpp) contains all mod metadata, settings, and code.

## Submit to the Windhawk mods collection

The mod file follows the [windhawk-mods](https://github.com/ramensoftware/windhawk-mods) layout: `mods/<mod-id>.wh.cpp`. To publish it, open a pull request that adds the single file `mods/keyboard-layout-text-converter.wh.cpp` to that repository.
