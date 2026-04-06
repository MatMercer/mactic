# haptic

Command-line tool to send haptic waveforms to the Force Touch trackpad on MacBooks.

Uses Apple's private `MultitouchSupport.framework` to directly drive the haptic actuator — no Xcode project or Swift required.

## Requirements

- MacBook with Force Touch trackpad (2015+)
- macOS (tested on M3 MacBook Pro)
- Xcode Command Line Tools (`xcode-select --install`)

## Build

```
make
```

## Usage

```
haptic [options]

Options:
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
  -f            Listen: stream live touch data (position, pressure, etc.)
  -l            List: actuate waveforms 1-20 with a pause between each
  -s            Scan: list multitouch devices and exit
  -h            Show this help
```

## Examples

```bash
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

# stream live touch data (ctrl-c to stop)
./haptic -f
```

### Listen mode output

`-f` streams per-frame touch data including:

- **position** — normalized (0-1) and absolute (mm)
- **pressure** — total pressure and Z-axis pressure
- **velocity** — normalized and absolute (mm/s)
- **touch geometry** — size, ellipse angle, major/minor axis, density
- **state** — touch phase (start, touch, press, lift, etc.)
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

Dynamic loading is used instead of direct `extern` declarations because arm64e pointer authentication can break direct calls into private frameworks.

The device ID is read from a known offset in the opaque `MTDevice` struct rather than calling `MTDeviceGetDeviceID`, which has an unstable calling convention across macOS versions.

## Caveats

- Uses **private API** — may break with any macOS update.
- The `MTDevice` struct layout (device ID at offset 64) was determined empirically and may differ across macOS versions.
- Waveform IDs beyond 6 may or may not work depending on firmware. Use `-l` to discover what works on your hardware.

## License

Public domain.
