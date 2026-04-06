# Implementation

Complete technical documentation of how `mactic` works, from framework loading to terminal rendering.

## Architecture overview

`mactic` is a single-file C program (~660 lines) that interfaces with Apple's private `MultitouchSupport.framework` to both send haptic waveforms and read raw multitouch data from the Force Touch trackpad. It has four operating modes:

1. **Actuate** (default) — send a haptic waveform
2. **Listen** (`-f`) — stream raw touch data to stdout
3. **ASCII viewer** (`-a`) — real-time braille heatmap in the terminal
4. **Scan** (`-s`) — enumerate multitouch devices

All modes share a common framework loading and device discovery layer.

## Framework loading

### Why dlopen instead of extern

The first major design decision. On arm64e Macs (Apple Silicon), the system enforces pointer authentication (PAC) on function pointers. When you declare a private framework function with `extern`, the linker resolves it at load time, but the resulting pointer may have incorrect PAC signatures because the framework's internal calling convention metadata isn't available in any public header. This causes bus errors (SIGBUS) at call time.

The fix: load `MultitouchSupport.framework` at runtime via `dlopen`/`dlsym`. Function pointers obtained through `dlsym` bypass PAC validation, giving us callable addresses regardless of the framework's internal metadata.

```c
mt_handle = dlopen("/System/Library/PrivateFrameworks/"
                   "MultitouchSupport.framework/MultitouchSupport", RTLD_LAZY);
pMTActuatorActuate = dlsym(mt_handle, "MTActuatorActuate");
```

The framework binary itself doesn't exist on disk on modern macOS — the symlink at `Versions/Current/MultitouchSupport` is broken. The actual code lives in the dyld shared cache (`/System/Library/dyld/dyld_shared_cache_arm64e`), and `dlopen` knows how to resolve it from there.

### Symbols loaded

Eight function pointers are resolved:

| Symbol | Purpose |
|--------|---------|
| `MTDeviceCreateList` | Returns a `CFMutableArrayRef` of all multitouch devices |
| `MTActuatorCreateFromDeviceID` | Creates a haptic actuator handle from a device ID |
| `MTActuatorOpen` / `MTActuatorClose` | Opens/closes the actuator for use |
| `MTActuatorActuate` | Fires a haptic waveform |
| `MTRegisterContactFrameCallback` | Registers a touch data callback |
| `MTDeviceStart` / `MTDeviceStop` | Starts/stops touch event delivery |

The actuator symbols are required; the device/touch symbols are optional (only needed for `-f`, `-a`, `-s` modes).

## Device discovery

### The device ID problem

`MTActuatorCreateFromDeviceID` needs the device's internal 64-bit ID. This is not a small index like 0, 1, 2 — it's a large number like `504403158265495761` (0x07000000000000d1).

The obvious approach is to call `MTDeviceGetDeviceID(device)` on each device from `MTDeviceCreateList()`. This crashes with SIGBUS. The function exists (confirmed via `dyld_info -exports`), but its calling convention is unknown — it might take an output pointer, use a different register convention, or have PAC issues even through dlsym.

### The struct offset solution

Instead of calling the function, we read the device ID directly from the opaque `MTDevice` struct at a known byte offset.

The offset was found empirically by:

1. Getting the real device ID from IORegistry: `ioreg -l -n AppleMultitouchTrackpad | grep "mt-device-id"`
2. Calling `MTDeviceCreateList()` and getting a pointer to the first device
3. Scanning the first 512 bytes of the struct for the known value

```c
#define MTDEVICE_ID_OFFSET 64

static uint64_t mt_device_get_id(void *dev) {
    uint64_t devID;
    memcpy(&devID, (uint8_t *)dev + MTDEVICE_ID_OFFSET, sizeof(devID));
    return devID;
}
```

`memcpy` is used instead of a cast to avoid alignment and strict-aliasing issues.

**This offset (64) was determined on an M3 MacBook Pro running macOS Sequoia. It may differ on other hardware or OS versions.** See `docs/future-proofing.md` for how to re-probe it.

### Haptic capability detection

Not every multitouch device has a haptic actuator (e.g., an external Magic Trackpad 1 won't). The scan iterates all devices, tries to create and open an actuator for each, and returns the first one that succeeds:

```
for each device in MTDeviceCreateList():
    id = read device ID from struct offset 64
    actuator = MTActuatorCreateFromDeviceID(id)
    if actuator && MTActuatorOpen(actuator) == success:
        this is our device
```

## Touch data struct (MTTouch)

The touch callback receives a pointer to a contiguous array of touch structs, one per active finger. The struct layout was determined by:

1. Registering a callback via `MTRegisterContactFrameCallback`
2. Dumping the raw bytes of each touch at 4-byte intervals
3. Interpreting values by cross-referencing with physical actions (moving finger, pressing harder, etc.)

### Struct layout (96 bytes per finger)

```
Offset  Size   Type     Field         Notes
──────  ────   ────     ─────         ─────
+0      4      int32    frame         Frame counter (matches callback arg)
+4      4      int32    (padding)     Always 0
+8      8      double   timestamp     Mach absolute time in seconds
+16     4      int32    pathIndex     Finger tracking ID (stable across frames)
+20     4      int32    state         Touch phase (see below)
+24     4      int32    fingerID      Finger classifier
+28     4      int32    handID        Hand classifier
+32     4      float    norm_x        Normalized X position (0.0–1.0)
+36     4      float    norm_y        Normalized Y position (0.0–1.0)
+40     4      float    vel_x         Normalized velocity X
+44     4      float    vel_y         Normalized velocity Y
+48     4      float    size          Contact area (~0.3 light → ~1.35 hard)
+52     4      float    pressure      Cumulative pressure (not instantaneous!)
+56     4      float    angle         Contact ellipse angle (radians)
+60     4      float    majorAxis     Contact ellipse major axis (mm)
+64     4      float    minorAxis     Contact ellipse minor axis (mm)
+68     4      float    density       Touch density (negative values observed)
+72     4      float    abs_x         Absolute X position (mm from left edge)
+76     4      float    abs_vel_x     Absolute velocity X (mm/s)
+80     4      float    abs_vel_y     Absolute velocity Y (mm/s)
+84     4      int32    (reserved)    Always 0
+88     4      int32    (reserved)    Always 0
+92     4      float    zPressure     Z-axis pressure (caps at ~1.54)
```

### Touch states

| Value | Name  | Meaning |
|-------|-------|---------|
| 0     | none  | Not tracking |
| 1     | start | First detection |
| 2     | hover | Finger near but not touching |
| 3     | make  | Initial contact |
| 4     | touch | Sustained contact |
| 5     | press | Force Touch threshold crossed |
| 6     | tap   | Brief contact |
| 7     | lift  | Finger leaving surface |

### Pressure fields

Three fields relate to pressure, each with different behavior:

- **`size` (+48)**: Best analog for instantaneous force. Ranges from ~0.3 (light touch) to ~1.35 (hard press). Good dynamic range. This is what the ASCII viewer uses for its heatmap color.

- **`pressure` (+52)**: Cumulative. Starts at 0 and continuously increases while touching (observed going 0 → 540+). Not useful for instantaneous pressure reading.

- **`zPressure` (+92)**: Caps at approximately 1.54 regardless of how hard you press. Poor dynamic range — doesn't differentiate between medium and hard presses.

These findings were determined by pressing the trackpad with increasing force and observing which fields responded proportionally.

## Haptic actuation

### Waveform IDs

`MTActuatorActuate(actuator, waveformID, 0, 0, 0)` fires a specific haptic pattern. The last three arguments are always 0 in our usage (their purpose is unknown).

Known waveform IDs:

| ID | Sensation |
|----|-----------|
| 1  | Weak click |
| 2  | Strong click (Force Touch feel) |
| 3  | Buzz / notification |
| 4  | Light tap |
| 5  | Medium tap |
| 6  | Strong tap |
| 15 | Soft thud |
| 16 | Strong thud |

IDs beyond ~16 are firmware-dependent. The `-l` flag cycles through 1–20 so you can feel what's available on your hardware.

### Actuator lifecycle

```
actuator = MTActuatorCreateFromDeviceID(deviceID)  // create handle
MTActuatorOpen(actuator, 0)                        // prepare for use
MTActuatorActuate(actuator, waveform, 0, 0, 0)     // fire pattern
MTActuatorClose(actuator)                          // release
CFRelease(actuator)                                // free CF object
```

The actuator is a Core Foundation type (CFTypeRef) and must be released.

## Listen mode (-f)

Registers a contact frame callback and runs a CFRunLoop:

```
MTRegisterContactFrameCallback(device, callback)
MTDeviceStart(device, 0)
CFRunLoopRunInMode(...)  // blocks, callback fires on touch events
MTDeviceStop(device)
```

The callback fires at the trackpad's polling rate (~125 Hz / 8ms) whenever fingers are on the surface. It receives:
- `device` — the MTDevice pointer
- `touches` — array of MTTouch structs
- `nFingers` — count of active fingers
- `timestamp` — mach absolute time
- `frame` — monotonic frame counter

Each invocation prints all finger data to stdout.

### Framework noise suppression

`MTDeviceStart` prints `*** Recognized (0x6f) family*** (30 cols X 22 rows)` to stdout/stderr the first time it's called. This is internal debug output from the framework.

We suppress it by temporarily redirecting stdout and stderr to `/dev/null` around the call:

```c
int so = dup(STDOUT_FILENO), se = dup(STDERR_FILENO);
int nul = open("/dev/null", O_WRONLY);
dup2(nul, STDOUT_FILENO);
dup2(nul, STDERR_FILENO);
pMTDeviceStart(dev, 0);
dup2(so, STDOUT_FILENO);
dup2(se, STDERR_FILENO);
```

## ASCII pressure viewer (-a)

The most complex mode. Renders a real-time braille heatmap of the trackpad surface in the terminal.

### Canvas geometry

The trackpad is 155mm × 99mm. The canvas must preserve this aspect ratio in the terminal.

Terminal characters are roughly twice as tall as they are wide (~8px × 16px). Each braille character encodes a 2×4 dot grid. So each braille dot occupies ~4px × 4px — effectively square. This means braille dots map 1:1 to physical proportions without correction.

Given a canvas width of `W` characters:
- Braille dot width: `W × 2`
- Braille dot height: `H × 4` where `H = W × (99/155) / 2`
- Each dot represents `155 / (W×2)` mm ≈ 1.1mm for a 70-char-wide display

The canvas auto-sizes to fill the terminal (detected via `ioctl(TIOCGWINSZ)`), capped at 140 chars wide and constrained by terminal height.

### Braille encoding

Unicode braille characters (U+2800–U+28FF) encode an 8-dot pattern in a 2-column × 4-row grid. The character codepoint is `0x2800 + bitfield`:

```
Position → Bit
(0,0) → 0    (1,0) → 3
(0,1) → 1    (1,1) → 4
(0,2) → 2    (1,2) → 5
(0,3) → 6    (1,3) → 7
```

For each character cell, we check 8 braille-dot positions against the heat buffer. If a dot's heat value exceeds the threshold (0.02), that bit is set. The resulting codepoint is UTF-8 encoded as 3 bytes (all braille chars are in the U+2800–U+28FF range):

```c
char utf[4] = {
    0xE0 | (cp >> 12),
    0x80 | ((cp >> 6) & 0x3F),
    0x80 | (cp & 0x3F), 0
};
```

### Heat buffer

`a_heat[y][x]` is a 2D float array at braille-dot resolution. Values range from 0.0 (no touch) to 1.0 (maximum pressure).

Each frame (at 30fps via CFRunLoopTimer):

1. **Decay**: Every cell is multiplied by 0.05 (95% reduction per frame). This gives near-instant fade matching the trackpad's 8ms reporting latency. A touch disappears within 1–2 frames of lifting.

2. **Paint**: For each active finger, a gaussian ellipse is stamped onto the heat buffer using the touch's physical geometry (majorAxis, minorAxis, angle) and pressure (size field).

3. **Render**: The heat buffer is converted to colored braille characters and written to the terminal.

### Gaussian ellipse rendering

Each touch is rendered as a soft blob, not a hard-edged ellipse. For every dot within the bounding box of the touch:

```c
// Rotate into ellipse-local coordinates
rx = dx * cos(-angle) - dy * sin(-angle)
ry = dx * sin(-angle) + dy * cos(-angle)

// Normalized distance from center (0=center, 1=edge)
d² = (rx/sa)² + (ry/sb)²

// Gaussian falloff
value = pressure_normalized × exp(-d² × 1.5)
```

Where `sa` and `sb` are the semi-axes in dot units, computed from the physical axes in mm divided by the mm-per-dot ratio. The axes are inflated by 1.3× for visibility, with a minimum of 2 dots.

The gaussian (`exp(-d² × 1.5)`) gives a smooth falloff: full intensity at the center, ~22% at the ellipse edge (d²=1), negligible beyond 2× the radius. This creates the soft, organic look of the heatmap.

### Auto-scaling pressure

The `size` field's range (~0.3 to ~1.35) determines the heat colormap intensity. `a_pmax` tracks the observed maximum to normalize pressure to 0.0–1.0.

Problem: if you slam your palm down, `a_pmax` jumps (e.g., to 9.0) and subsequent normal touches are invisible.

Solution: `a_pmax` decays toward a baseline of 1.4 each frame:

```c
a_pmax = a_pmax * 0.97 + 1.4 * 0.03;
```

This is an exponential moving average. After a palm event, `a_pmax` recovers to usable levels in ~1–2 seconds (50–60 frames). The baseline of 1.4 was chosen because a hard single-finger press produces `size ≈ 1.35`.

### Color mapping

The heat colormap maps a normalized value (0.0–1.0) to an RGB color via a piecewise linear function through 6 control points:

```
0.00 → (  0,   0,   0)  black      (no touch)
0.12 → (  0,   0, 200)  deep blue  (faintest touch)
0.25 → (  0, 180, 255)  cyan
0.42 → (  0, 255,   0)  green
0.60 → (255, 255,   0)  yellow
0.80 → (255,   0,   0)  red
1.00 → (255, 255, 255)  white      (maximum pressure)
```

Colors are applied using 24-bit truecolor ANSI escape sequences (`\033[38;2;R;G;Bm`). The renderer tracks the previous color and only emits a new escape sequence when it changes, reducing output size.

### Flicker-free rendering

The entire frame is built in a 256KB static buffer (`ob[]`) using `ob_s()` (string append) and `ob_f()` (printf append), then flushed to stdout with a single `write()` syscall. This avoids the tearing that would occur if individual characters were written separately.

Terminal control:
- **Alternate screen buffer** (`\033[?1049h`): The viewer runs in a separate screen buffer so the normal terminal content is preserved when exiting.
- **Cursor hiding** (`\033[?25l`): Prevents cursor flicker during redraws.
- **Cursor home** (`\033[H`): Each frame redraws from the top-left instead of scrolling.
- **Line clearing** (`\033[K`, `\033[J`): Clears any leftover content from previous frames.

### Architecture (callback vs timer)

Touch data arrives via the contact frame callback at ~125 Hz. But rendering at 125 fps is wasteful — terminals can't display that fast and the output would be enormous.

The solution splits input from rendering:

- **Touch callback** (`ascii_touch_cb`): Fires at 125 Hz. Does nothing except copy the latest touch data into a shared buffer (`a_touches[]`, `a_nfingers`). Runs on the CFRunLoop's main thread.

- **Render timer** (`ascii_timer_cb`): Fires at 30 fps via `CFRunLoopTimerCreate`. Handles decay, painting, and rendering. Also runs on the main thread, serialized with the callback (no concurrency issues).

This ensures smooth 30fps output regardless of the input rate, and the heat buffer continues to decay even when no fingers are touching (so residual heat fades after lifting).

### Cleanup

On SIGINT (ctrl-c):
1. `g_running` flag is set to 0
2. `CFRunLoopStop()` is called from the signal handler to unblock the main loop
3. The timer is removed and the device is stopped
4. Terminal is restored: cursor shown (`\033[?25h`), alternate screen exited (`\033[?1049l`)

## Signal handling

Both listen and ASCII modes install a SIGINT handler. The handler sets a flag and calls `CFRunLoopStop()` to wake the runloop immediately (otherwise we'd wait up to 1 second for the timeout). `g_running` is declared `volatile sig_atomic_t` for signal safety.

## Build system

The Makefile is minimal:

```make
CC      = clang
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -F/System/Library/PrivateFrameworks/ \
          -framework MultitouchSupport \
          -framework CoreFoundation \
          -framework IOKit
```

- `-F` adds the private frameworks directory to the framework search path
- `-framework MultitouchSupport` links against the private framework (even though we dlopen it at runtime, the linker needs it for the framework dependency chain)
- CoreFoundation is needed for CFArray, CFTypeRef, CFRunLoop
- IOKit is needed for IOReturn and kIOReturnSuccess
- `math.h` functions (cosf, sinf, expf) are in libSystem on macOS, no `-lm` needed
