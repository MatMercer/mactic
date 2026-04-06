# Future-proofing

This tool depends on Apple's private `MultitouchSupport.framework`. Private APIs are undocumented, unsupported, and can change without notice in any macOS update. This document explains what can break and how to fix it.

## What can break

### 1. MTDevice struct layout changes

The tool reads the device ID from a hardcoded offset (`MTDEVICE_ID_OFFSET = 64`) inside the opaque `MTDevice` struct. If Apple adds, removes, or reorders fields, this offset will be wrong and the tool will either crash or use the wrong device.

**Symptoms:** wrong device ID, `MTActuatorCreateFromDeviceID` returns NULL, or a bus error on startup.

**How to fix:**

Run the probe to find the new offset. You need the device ID from `ioreg` to know what to look for:

```bash
# Get the real device ID
ioreg -l -n AppleMultitouchTrackpad | grep "mt-device-id"

# Then compile and run the probe (see below) and search for that value in the struct dump
```

Probe code — dumps the raw bytes of the MTDevice struct so you can find where the device ID moved:

```c
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_LAZY);
    CFMutableArrayRef (*createList)(void) = dlsym(h, "MTDeviceCreateList");
    CFMutableArrayRef devs = createList();
    void *dev = (void *)CFArrayGetValueAtIndex(devs, 0);

    uint8_t *bytes = (uint8_t *)dev;
    for (int off = 0; off < 512; off += 8) {
        uint64_t val;
        memcpy(&val, bytes + off, 8);
        printf("+%3d: 0x%016llx\n", off, (unsigned long long)val);
    }
    CFRelease(devs);
    return 0;
}
```

Match the output against the `mt-device-id` from `ioreg`. Update `MTDEVICE_ID_OFFSET` in `haptic.c` to the new offset.

### 2. MTTouch struct layout changes

The listen mode (`-f`) reads touch data from a 96-byte struct whose layout was determined empirically. If Apple changes the struct size or field positions, the reported values will be garbage.

**Symptoms:** nonsensical position/pressure values, fields that are always zero or NaN.

**How to fix:**

Use a similar probe approach — register a contact frame callback and dump the raw bytes of each touch:

```c
// Inside a touch callback:
uint8_t *base = (uint8_t *)touches;
for (int i = 0; i < 128; i += 4) {
    float fval;
    int32_t ival;
    memcpy(&fval, base + i, 4);
    memcpy(&ival, base + i, 4);
    printf("+%3d: int=%-12d  float=%.6f\n", i, ival, fval);
}
```

Touch the trackpad and look for:
- **Frame number** — should match the `frame` callback argument (usually at offset 0)
- **Normalized position** — floats between 0 and 1 that change as you slide your finger
- **Pressure** — a float that increases when you press harder
- **Velocity** — floats that are zero when still, nonzero when moving

Update the `MTTouch` struct definition in `haptic.c` accordingly.

### 3. Function signatures change

If Apple changes the arguments or return types of functions like `MTActuatorActuate` or `MTRegisterContactFrameCallback`, calls will silently pass wrong values or crash.

**Symptoms:** bus error or segfault inside a framework call, `MTActuatorOpen` returning unexpected error codes.

**How to fix:**

Disassemble the function to check its expected arguments:

```bash
# List all exported symbols
dyld_info -exports /System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport | grep MTActuator
```

If the binary is only in the dyld shared cache (no on-disk binary to disassemble), you can use `lldb`:

```bash
lldb -o "image lookup -rn MTActuatorActuate" -o "quit"
```

Or write a minimal test program that calls the function with known arguments and check for crashes.

### 4. Framework removed or renamed

Apple could remove `MultitouchSupport.framework` entirely or fold it into another framework.

**Symptoms:** `dlopen` returns NULL.

**How to fix:**

Search for where the symbols moved:

```bash
# Search the shared cache for the symbol
dyld_info -exports /System/Library/dyld/dyld_shared_cache_arm64e 2>&1 | grep MTActuator
```

If the symbols still exist under a different framework path, update `MT_FW` in `haptic.c`.

### 5. Waveform IDs change

The waveform IDs (1-6, 15, 16) are firmware-level constants. A firmware update could renumber them or add/remove patterns.

**Symptoms:** waveforms feel different than expected, or `MTActuatorActuate` returns errors for previously working IDs.

**How to fix:**

Use `./mactic -l` to cycle through IDs 1-20 and re-map which ID produces which sensation. There's no programmatic way to query waveform names — you have to feel them.

## General debugging tips

- **Bus errors** almost always mean a struct offset or function signature is wrong. Start by re-probing the struct layout.
- **dlsym returning NULL** means the symbol was renamed or removed. Check `dyld_info -exports` for the current symbol list.
- **Silent wrong behavior** (wrong device, garbled touch data) means struct layouts shifted. Re-run the probes.
- Apple tends to keep private API changes within major macOS releases. Always re-test after upgrading macOS.
