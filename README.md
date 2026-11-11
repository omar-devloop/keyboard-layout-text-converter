# Keyboard Layout Text Converter (Windhawk mod)

A [Windhawk](https://windhawk.net/) mod that converts text typed with the wrong keyboard layout in place, across most Windows applications.

Select the mistyped text and press **Left Ctrl + Left Shift**. The mod copies the selection, remaps it by key positions between the keyboard layouts installed on your device, and pastes the converted text back.

## Features

- Converts a selection with a global shortcut, with an optional previous-word fallback.
- Detects the most plausible source/target layout among the installed layouts, or cycles through them.
- Supports Latin, Cyrillic, Hebrew, Arabic, and Greek layouts for conversion scoring.
- Optional clipboard restore and optional keyboard-layout switch after conversion.
- Optional small success toast in the screen corner (off by default).
- Runs inside `explorer.exe`.

## Install

1. Install [Windhawk](https://windhawk.net/).
2. In Windhawk, create a new mod from source and paste the contents of [`mods/keyboard-layout-text-converter.wh.cpp`](mods/keyboard-layout-text-converter.wh.cpp).
3. Compile and enable the mod.

## Usage

- Select text and press the conversion shortcut (default `LCtrl+LShift`).
- With the default *selection-first* scope, pressing the shortcut without a selection converts the previous word.
- Regular combinations such as `Ctrl+Alt+Space` are also supported. The shortcut setting accepts `L`/`R` prefixes to pick a specific side (e.g. `LCtrl+LShift`, `RCtrl+F12`).

## Settings

- **Conversion shortcut** — global shortcut; modifier-only combinations are supported.
- **Conversion mode** — auto-detect layouts or cycle through installed layouts.
- **Capture scope** — selection with previous-word fallback, selection only, or previous word only.
- **Restore clipboard** — restore the previous clipboard text after conversion.
- **Show notification** - show a small success toast in the screen corner after conversion (off by default).
- **Switch keyboard layout after convert** — switch to the detected target layout.
- **Clipboard timeout (ms)** — base timeout for clipboard copy/paste handshakes; raise it for slow applications.

## Notes and limitations

- Replacement is clipboard-based (simulated copy/paste), so behavior can vary in apps with custom clipboard/shortcut handling.
- The mod may not interact with elevated (UIPI) windows, secure fields, or some terminals/remote apps.
- Modifier-only shortcuts only fire when the keys are pressed and released on their own. Pressing another key (e.g. `Ctrl+Shift+T`) cancels the shortcut, so normal `Ctrl+Shift+...` shortcuts keep working.
- Very short or ambiguous text can be hard to auto-detect; use a longer selection or adjust settings if needed.

## Source

[`mods/keyboard-layout-text-converter.wh.cpp`](mods/keyboard-layout-text-converter.wh.cpp) contains all mod metadata, settings, and code.

## Submit to the Windhawk mods collection

The mod file follows the [windhawk-mods](https://github.com/ramensoftware/windhawk-mods) layout: `mods/<mod-id>.wh.cpp`. To publish it, open a pull request that adds the single file `mods/keyboard-layout-text-converter.wh.cpp` to that repository.
