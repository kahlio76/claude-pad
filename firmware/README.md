# Pico firmware

The firmware is deliberately small. It does three jobs and nothing else:
turn switch presses into `D Approve` / `U Approve` lines, read `L wait work off
off 1` lines back, and show them on four LEDs. All of the deciding stays on the
Mac, in `padd` and the C core.

```
firmware/
  src/pad_fw.[ch]    the logic: debounce, line protocol, LED colors. No hardware.
  tests/test_fw.c    42 tests for it, run on your laptop (make fw-test)
  pico/              the thin Pico layer: pins, serial, LEDs. Builds for the RP2040.
    main.c           which pin is which, and the 1 kHz loop
    CMakeLists.txt
    ws2812.pio       LED driver from raspberrypi/pico-examples (BSD-3-Clause)
    wokwi.toml  diagram.json   the simulator project
```

The key names and numbering come from `Sources/PadCore/include/pad_core.h`, the
same header `padd` uses, and the build compiles `core.c` in too. The pad and
the daemon can't disagree about what a key is called.

## Wiring

One GPIO per switch, switch to ground, internal pull-up (nothing else needed).

| Key | GPIO | Key | GPIO | Key | GPIO |
|---|---|---|---|---|---|
| S1 | 2 | Approve | 6 | Talk | 10 |
| S2 | 3 | Always | 7 | Stop | 11 |
| S3 | 4 | Deny | 8 | FN | 12 |
| S4 | 5 | Mode | 9 | | |

Four WS2812/SK6812 LEDs chained on **GP16** (one per session slot). Power them
from VBUS (5 V). The Pico's 3.3 V data line is technically below their spec of
0.7 x VDD, and works in practice for short runs; a 74AHCT125 level shifter, or
a diode in series with the LED supply, makes it by the book.

LED colors: dim white = idle, amber = working, green = done, **red blinking =
Claude is asking permission**. The selected slot is bright; the others dimmed.
A waiting slot stays bright even when it isn't selected.

## Build

You need CMake, the [Arm GNU toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
(`brew install --cask gcc-arm-embedded` on a Mac, which asks for your password;
Homebrew's plain `arm-none-eabi-gcc` formula has no C library and won't link),
and the [Pico SDK](https://github.com/raspberrypi/pico-sdk) with submodules.

```sh
git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/raspberrypi/pico-sdk ~/pico-sdk
export PICO_SDK_PATH=~/pico-sdk

cd firmware/pico
cmake -S . -B build
cmake --build build -j8
```

That produces `build/claude_pad.uf2` (about 28 KB). Verified with SDK 2.3.1 and
Arm GNU Toolchain 15.3.rel2: no warnings from this repo's code.

## Flash a real Pico

Hold BOOTSEL while plugging in the board. It shows up as a drive called
`RPI-RP2`; drag `claude_pad.uf2` onto it and it reboots into the firmware. Find
its serial port (`ls /dev/cu.usbmodem*`) and point the daemon at it:

```sh
~/.claude-pad/bin/padd --serial /dev/cu.usbmodem1101
```

## Simulate it in Wokwi

Install the [Wokwi for VS Code](https://docs.wokwi.com/vscode/getting-started)
extension, build the firmware (above), open `firmware/pico` in VS Code, and run
**Wokwi: Start Simulator** from the command palette (F1). `wokwi.toml` points at
the build output and forwards the simulated serial port on
`rfc2217://localhost:4000`.

Status: `diagram.json` and `wokwi.toml` follow Wokwi's documented format and are
checked for dangling wires, but **haven't been run in the simulator yet**. If
Wokwi complains about a pin name, the fix will be a one-line edit in
`diagram.json`. The simulated Pico's serial output goes over UART0; whether the
USB side is simulated I haven't confirmed, which is why the firmware writes to
both.

## Not done yet

- **Bridging the simulator to `padd`.** Wokwi's serial arrives over RFC2217
  (a TCP protocol), and `padd --serial` expects a device file. A small bridge
  that exposes the RFC2217 port as a pseudo-terminal is the missing piece.
- **No "disconnected" LED state.** The Mac only sends LED lines when something
  changes, so the pad can't tell the daemon has gone away. `padd` re-sends the
  current state whenever the pad connects.
