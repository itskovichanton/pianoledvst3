# PianoLED

AU/VST3 инструмент: MIDI с трека зажигает светодиоды на ленте ESP32-C6. Звук
рояля — отдельный трек (Steinway); PianoLED звук не генерирует.

Раскладка равномерная: **3 LED на клавишу**, 144 светодиода = MIDI **C2–B5** (ноты 36–83). Остальные ноты игнорируются.

Форматы: **AU**, **VST3**, **Standalone**. Мост USB — [`bridge/`](bridge/) (PianoLedBridge).

## Лента без DAW (сначала это)

Прошивка ESP32-C6, GPIO4, 144 × WS2812. Монитор USB не открывать — по тому же порту идут кадры.

```bash
cd bridge/firmware
./flash.sh

cd ../mac
./run_ledctl.sh ping
./run_ledctl.sh scale
```

Гамма побежала — железо исправно. **Закрой ledctl** перед запуском плагина: порт один.

## Сборка плагина

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
cmake --preset debug
cmake --build --preset standalone
cmake --build --preset vst3
cmake --build --preset au
```

Или:

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target PianoLED_Standalone PianoLED_VST3 PianoLED_AU -j
```

## Logic

После сборки AU:

```text
~/Library/Audio/Plug-Ins/Components/PianoLED.component
```

1. Закрой `ledctl`, подключи плату.
2. Logic / GarageBand: **два трека** с одним MIDI — Steinway (звук) и AU **PianoLED** (лента).
3. Play MIDI в диапазоне **C2–B5**.
4. В окне плагина: зелёная строка `LED strip: …` и ноты аккорда; на ленте зажигаются тройки светодиодов.

### GarageBand

PianoLED — **AU-инструмент** (зелёный слот), не эффект. В синих Плагинах (EQ, компрессор) MIDI нет.

Чтобы слышать Steinway и видеть плагин:

1. Полностью закрой GarageBand (иначе кэш старого AU).
2. Трек 1: Steinway, MIDI-регион — это звук.
3. Track → New Track → Software Instrument. В **зелёном** слоте: **AU Instruments → PianoLED → PianoLED**. Не Magenta.
4. Скопируй тот же MIDI-регион на трек 2. Громкость трека 2 — в ноль (Mute), чтобы не было тишины в миксе как «дыры».
5. С трека 1 сними PianoLED, если он висит в синих эффектах.
6. Play **с начала** региона.

Песочница AU не пускает плагин в `/dev/cu.usbmodem*`. Сборка ставит хелпер
`~/Library/Application Support/PianoLED/PianoLEDBridge.app` — он открывает USB,
а плагин ходит к нему по localhost.

1. Закрой `ledctl` и `idf.py monitor`.
2. Если лента не нашлась и строка не зелёная — нажми **«Подключить ленту»**. Если не помогло:

```bash
open -g "$HOME/Library/Application Support/PianoLED/PianoLEDBridge.app"
```

3. Играй **C2–B5**. A#1 ниже диапазона, пока трек не транспонирован на +12.

## Standalone

```bash
open "build/PianoLED_artefacts/Debug/Standalone/PianoLED.app"
```

## VST3 (если нужен другой хост)

```text
~/Library/Audio/Plug-Ins/VST3/PianoLED.vst3
```
