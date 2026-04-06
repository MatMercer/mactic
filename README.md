# haptic

Command-line tool to send haptic waveforms and visualize multitouch input on the Force Touch trackpad of MacBooks.

Uses Apple's private `MultitouchSupport.framework` to directly drive the haptic actuator and read raw touch data — no Xcode project or Swift required.

## Requirements

- MacBook with Force Touch trackpad (2015+)
- macOS (tested on M3 MacBook Pro, macOS Sequoia)
- Xcode Command Line Tools (`xcode-select --install`)

## Build

```
make
```

## Usage

```
haptic [options]

Options:
  -a            ASCII pressure viewer (braille heatmap in the terminal)
  -f            Listen: stream live touch data (position, pressure, etc.)
  -w <id>       Waveform ID to actuate (default: 2)
                  1 = weak click
                  2 = strong click (Force Touch feel)
                  3 = buzz / notification
                  4 = light tap
                  5 = medium tap
                  6 = strong tap
  -d <deviceID> Multitouch device ID (default: auto-detect)
  -r <count>    Repeat count (default: 1)
  -i <ms>       Interval between repeats in milliseconds (default: 200)
  -l            List: actuate waveforms 1-20 with a pause between each
  -s            Scan: list multitouch devices and exit
  -h            Show this help
```

## Examples

```bash
# ascii pressure heatmap (esc/q to quit)
./haptic -a

# stream live touch data (esc/q to quit)
./haptic -f

# single strong click
./haptic -w 2

# buzz three times
./haptic -w 3 -r 3

# rapid light taps
./haptic -w 4 -r 5 -i 100

# feel all waveforms 1-20
./haptic -l

# scan for multitouch devices
./haptic -s
```

## ASCII pressure viewer

`-a` renders a real-time braille heatmap of the trackpad surface (155x99mm) in the terminal. Uses Unicode braille characters for 2x4 sub-cell dot resolution per character, with the canvas auto-sized to your terminal while preserving the physical aspect ratio.

Each touch renders as a gaussian ellipse using the actual contact geometry (major/minor axis and angle from the hardware). Pressure intensity is mapped to a heat colormap (blue → cyan → green → yellow → red → white) using the `size` field from the touch data, which provides the best dynamic range (~0.3 for a light touch to ~1.35 for a hard press).

The display:
- Runs at 30fps in an alternate screen buffer (your terminal content is preserved on exit)
- Near-instant heat decay matching the trackpad's 8ms reporting latency
- Auto-scaling pressure normalization that recovers after palm events
- Flicker-free rendering via single-syscall frame output (256KB buffer)
- Status bar with finger count, colored pressure meter, and per-finger coordinates
- Exit with ESC, q, or ctrl-c

## Listen mode

`-f` streams per-frame touch data at the trackpad's native ~125Hz polling rate, including:

- **position** — normalized (0-1) and absolute (mm)
- **pressure** — contact size, cumulative pressure, and Z-axis pressure
- **velocity** — normalized and absolute (mm/s)
- **touch geometry** — ellipse angle, major/minor axis (mm), density
- **state** — touch phase (start, hover, make, touch, press, tap, lift)
- **finger tracking** — path index, finger ID, hand ID

```
frame 38     t=981719.5870  fingers=1
  [4] state=touch  pos=(0.503, 0.336)  vel=(0.000, 0.000)  pressure=60.0  size=1.418  angle=1.09  axis=(11.14, 9.25)  density=3.09  abs_x=30.1mm  abs_vel=(0.0, 0.0)mm/s  zPressure=1.398
```

## How it works

The tool dynamically loads `MultitouchSupport.framework` (a private Apple framework) via `dlopen`/`dlsym` and calls:

1. `MTDeviceCreateList` — enumerates multitouch devices
2. `MTActuatorCreateFromDeviceID` — creates a haptic actuator handle
3. `MTActuatorOpen` / `MTActuatorActuate` / `MTActuatorClose` — drives the actuator
4. `MTRegisterContactFrameCallback` / `MTDeviceStart` / `MTDeviceStop` — reads touch data

Dynamic loading is used instead of direct `extern` declarations because arm64e pointer authentication (PAC) on Apple Silicon breaks direct calls into private frameworks, causing bus errors.

The device ID is read from a known byte offset (64) in the opaque `MTDevice` struct, because `MTDeviceGetDeviceID` has an unstable calling convention that also causes bus errors.

The 96-byte-per-finger touch struct layout was determined empirically by dumping raw bytes from the contact frame callback and cross-referencing field values with physical actions. See [docs/implementation.md](docs/implementation.md) for the full struct map and field analysis.

## Docs

- [docs/implementation.md](docs/implementation.md) — complete technical documentation of every subsystem
- [docs/future-proofing.md](docs/future-proofing.md) — what can break across macOS updates and how to fix it

## Caveats

- Uses **private API** — may break with any macOS update.
- The `MTDevice` struct layout (device ID at offset 64) and `MTTouch` struct layout (96 bytes) were determined empirically and may differ across macOS versions.
- Waveform IDs beyond 6 may or may not work depending on firmware. Use `-l` to discover what works on your hardware.
- The ASCII viewer requires a terminal with truecolor support (24-bit ANSI — most modern terminals including Terminal.app, iTerm2, kitty, etc.).

## License

Public domain.
