# Claude Pad

A physical keypad for Claude Code. Approve or deny permission requests,
interrupt, switch modes and juggle up to four sessions without touching the
terminal. Status LEDs show what each session is doing.

```
  pad (Pico)  ──key edges──▶   padd   ◀── permission request ──  Claude Code
              ◀──LED states──  (runs    ── allow / deny ───────▶  (hooks)
                               pad_core)
```

## Layout

| Path | What it is |
|---|---|
| `Sources/PadCore/` | The keypad logic in portable C11: what every press, hold and FN combo means. No allocation, no platform code. |
| `Sources/padd/` | The daemon (Swift). Owns the core, talks to the pad and to Claude Code. |
| `Sources/pad-hook/` | What Claude Code's hooks run. Forwards events to padd; for permission requests, waits for the pad's answer. |
| `Sources/fakepad/` | Your keyboard pretending to be the Pico, until the hardware exists. |
| `firmware/` | The Pico firmware: debounce, serial protocol and LED logic in plain C (`firmware/src`), plus the thin RP2040 layer and Wokwi project (`firmware/pico`). See [firmware/README.md](firmware/README.md). |
| `tests/test_core.c` | 20 behavior tests for the C core. |
| `firmware/tests/test_fw.c` | 42 tests for the firmware logic. Run both C suites with `make test`. |
| `tests/e2e.py` | 18 end-to-end tests: real padd, real pad-hook, scripted pad (`python3 tests/e2e.py`). |

## Try it with the fake pad

```sh
./install.sh                       # builds, copies binaries to ~/.claude-pad/bin
```

Merge `hooks/settings-hooks.json` into the `"hooks"` section of
`~/.claude/settings.json`. Then, in two terminal tabs:

```sh
~/.claude-pad/bin/padd             # tab 1: the daemon (leave it running)
~/.claude-pad/bin/fakepad          # tab 2: the pad
```

Start Claude Code in a third tab and ask it to do something that needs
permission. The fake pad shows `[S1 WAIT]`; press `a` to approve or `d` to deny.

Fake pad keys: lowercase taps, uppercase toggles a hold (terminals can't see
key releases). `1`-`4` select, `a` approve, `d` deny, `W` twice with a second
between = Always, `m` mode, `s` stop, `f` toggles FN, `q` quits.

## How it behaves

- **Approve / Deny** answer the exact request that was waiting when the key
  went down. If a new prompt replaces it mid-press, the press does nothing.
- **Always** (hold 1 s) allows the request and auto-allows that tool for the
  rest of the session. A quick tap does nothing on purpose.
- **Nothing ever blocks Claude.** If padd isn't running, hooks do nothing. If
  nobody answers within 60 s (`padd --timeout N`), Claude Code shows its normal
  prompt. A pad disconnect cancels held keys without approving anything.
- **Stop, Mode, FN+Stop, Talk tap** send Esc, Shift+Tab, Esc Esc and Return to
  the focused window, so keep Claude Code's terminal in front. macOS asks once
  for Accessibility permission for the terminal app running padd.
- **Not wired yet:** creating/closing sessions, the agents peek, Plan, and
  dictation. padd logs them as "not wired up yet".

## The real pad

The Pico firmware only reports debounced edges and shows LEDs, over USB
serial:

- pad → Mac: `D Approve` / `U Approve` (key names: S1-S4, Approve, Always,
  Deny, Mode, Talk, Stop, FN)
- Mac → pad: `L <S1> <S2> <S3> <S4> <selected>`, each slot one of
  `off idle work wait done`, selected as 1-4

Run `padd --serial /dev/cu.usbmodemXXXX` instead of using the fake pad.

The firmware builds and links for the RP2040 today (wiring, build steps and
flashing are in [firmware/README.md](firmware/README.md)). It hasn't run on a
physical board yet: the hardware is still on order.
