# PianoLED

AU/VST3 инструмент на JUCE 8: каркас под визуализацию нот пианино (LED).

Сейчас плагин слушает MIDI, не генерирует звук и показывает последнюю ноту в UI. Форматы сборки: **AU**, **VST3** и **Standalone**.

GarageBand не загружает MIDI FX (`aumi`) и не шлёт MIDI в обычные эффекты, поэтому PianoLED собран как **AU Music Device** (`aumu`) — его нужно выбирать как инструмент.

## Требования (macOS)

- Xcode (полный, не только Command Line Tools)
- CMake 3.24+
- Git

Если `xcode-select` указывает на Command Line Tools, перед сборкой задайте полный Xcode в этой сессии:

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
```

## Сборка

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
cmake --preset debug
cmake --build --preset standalone
cmake --build --preset vst3
cmake --build --preset au
```

Или без presets:

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target PianoLED_Standalone PianoLED_VST3 PianoLED_AU -j
```

JUCE подтягивается автоматически через CMake FetchContent (тег `8.0.15`).

## Запуск без DAW (Standalone)

```bash
open "build/PianoLED_artefacts/Debug/Standalone/PianoLED.app"
```

В меню приложения можно выбрать MIDI-вход. Сыграйте ноту — в окне появятся pitch и velocity.

## Установка в REAPER

После сборки VST3 (при `COPY_PLUGIN_AFTER_BUILD`) бандл копируется в:

```text
~/Library/Audio/Plug-Ins/VST3/PianoLED.vst3
```

1. Откройте REAPER → **Settings** (`Cmd+,`) → **Plug-ins → VST**.
2. Убедитесь, что в путях есть `~/Library/Audio/Plug-Ins/VST3`.
3. Включите VST3 и нажмите **Clear cache/re-scan** (или **Re-scan**).
4. На MIDI-треке откройте **FX** и добавьте **VSTi: PianoLED** (это инструмент, не обычный FX).
5. Направьте на трек MIDI-вход (клавиатура или Virtual MIDI Keyboard).

## Установка в GarageBand

После сборки AU копируется в:

```text
~/Library/Audio/Plug-Ins/Components/PianoLED.component
```

1. Закройте GarageBand, если он открыт.
2. Откройте GarageBand → новый проект → **Software Instrument**.
3. Нажмите на слот инструмента в Smart Controls (название пресета слева от дорожки).
4. В меню генератора выберите **AU Instruments → PianoLED → PianoLED**.
5. Если пункта нет: **GarageBand → Settings → Audio/MIDI** и включите **Enable Audio Units**, затем перезапустите GarageBand.
6. Сыграйте на MIDI-клавиатуре или Musical Typing (`Cmd+K`) — в окне плагина появятся ноты.

Трек будет без звука: это визуализатор, не синтезатор. Для звука пианино создайте вторую Software Instrument дорожку.
