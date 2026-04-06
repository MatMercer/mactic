#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

// MultitouchSupport private framework — loaded dynamically to get correct
// calling conventions on arm64e (pointer authentication can break extern decls).

#define MT_FW "/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport"

// Offset of the device ID field inside the opaque MTDevice struct.
// Found empirically on M3 MacBook — may change across macOS versions.
#define MTDEVICE_ID_OFFSET 64

// Touch data struct (96 bytes per finger, layout determined empirically)
typedef struct {
    int32_t  frame;          // +0
    int32_t  _pad0;          // +4
    double   timestamp;      // +8
    int32_t  pathIndex;      // +16: finger tracking ID
    int32_t  state;          // +20: touch phase
    int32_t  fingerID;       // +24
    int32_t  handID;         // +28
    float    norm_x;         // +32: normalized X (0-1)
    float    norm_y;         // +36: normalized Y (0-1)
    float    vel_x;          // +40: velocity X (normalized)
    float    vel_y;          // +44: velocity Y (normalized)
    float    size;           // +48: touch size
    float    pressure;       // +52: total pressure
    float    angle;          // +56: ellipse angle (radians)
    float    majorAxis;      // +60: ellipse major axis (mm)
    float    minorAxis;      // +64: ellipse minor axis (mm)
    float    density;        // +68: touch density
    float    abs_x;          // +72: absolute position (mm)
    float    abs_vel_x;      // +76: absolute velocity X (mm/s)
    float    abs_vel_y;      // +80: absolute velocity Y (mm/s)
    int32_t  _reserved1;     // +84
    int32_t  _reserved2;     // +88
    float    zPressure;      // +92: Z-axis pressure
} MTTouch;

static void *mt_handle;

// Function pointer types — actuator
typedef CFTypeRef (*MTActuatorCreateFromDeviceID_t)(uint64_t deviceID);
typedef IOReturn  (*MTActuatorOpen_t)(CFTypeRef actuator, uint32_t options);
typedef IOReturn  (*MTActuatorClose_t)(CFTypeRef actuator);
typedef IOReturn  (*MTActuatorActuate_t)(CFTypeRef actuator, int32_t waveform, uint32_t a1, uint32_t a2, uint32_t a3);

// Function pointer types — device & touch
typedef CFMutableArrayRef (*MTDeviceCreateList_t)(void);
typedef void (*MTContactCallbackFunction)(void *device, MTTouch *touches, int nFingers, double timestamp, int frame);
typedef void (*MTRegisterContactFrameCallback_t)(void *device, MTContactCallbackFunction callback);
typedef void (*MTDeviceStart_t)(void *device, int mode);
typedef void (*MTDeviceStop_t)(void *device);

static MTActuatorCreateFromDeviceID_t    pMTActuatorCreateFromDeviceID;
static MTActuatorOpen_t                  pMTActuatorOpen;
static MTActuatorClose_t                 pMTActuatorClose;
static MTActuatorActuate_t               pMTActuatorActuate;
static MTDeviceCreateList_t              pMTDeviceCreateList;
static MTRegisterContactFrameCallback_t  pMTRegisterContactFrameCallback;
static MTDeviceStart_t                   pMTDeviceStart;
static MTDeviceStop_t                    pMTDeviceStop;

static int load_mt(void) {
    mt_handle = dlopen(MT_FW, RTLD_LAZY);
    if (!mt_handle) {
        fprintf(stderr, "error: dlopen: %s\n", dlerror());
        return -1;
    }
    pMTActuatorCreateFromDeviceID  = dlsym(mt_handle, "MTActuatorCreateFromDeviceID");
    pMTActuatorOpen                = dlsym(mt_handle, "MTActuatorOpen");
    pMTActuatorClose               = dlsym(mt_handle, "MTActuatorClose");
    pMTActuatorActuate             = dlsym(mt_handle, "MTActuatorActuate");
    pMTDeviceCreateList            = dlsym(mt_handle, "MTDeviceCreateList");
    pMTRegisterContactFrameCallback = dlsym(mt_handle, "MTRegisterContactFrameCallback");
    pMTDeviceStart                 = dlsym(mt_handle, "MTDeviceStart");
    pMTDeviceStop                  = dlsym(mt_handle, "MTDeviceStop");

    if (!pMTActuatorCreateFromDeviceID || !pMTActuatorOpen ||
        !pMTActuatorClose || !pMTActuatorActuate) {
        fprintf(stderr, "error: could not resolve MTActuator symbols\n");
        return -1;
    }
    if (!pMTDeviceCreateList) {
        fprintf(stderr, "warning: could not resolve MTDeviceCreateList\n");
    }
    return 0;
}

static uint64_t mt_device_get_id(void *dev) {
    uint64_t devID;
    memcpy(&devID, (uint8_t *)dev + MTDEVICE_ID_OFFSET, sizeof(devID));
    return devID;
}

// Known waveform IDs (discovered via reverse engineering)
//  1 - weak click
//  2 - strong click (Force Touch click feel)
//  3 - buzz / notification
//  4 - light tap
//  5 - medium tap
//  6 - strong tap
// 15 - soft thud
// 16 - strong thud

static int64_t find_trackpad_device_id(int verbose) {
    if (!pMTDeviceCreateList) {
        fprintf(stderr, "error: MTDeviceCreateList not available\n");
        return -1;
    }

    CFMutableArrayRef devices = pMTDeviceCreateList();
    if (!devices) {
        fprintf(stderr, "error: MTDeviceCreateList returned NULL\n");
        return -1;
    }

    CFIndex count = CFArrayGetCount(devices);
    if (verbose)
        printf("Found %ld multitouch device(s):\n", (long)count);

    int64_t found_id = -1;
    for (CFIndex i = 0; i < count; i++) {
        void *dev = (void *)CFArrayGetValueAtIndex(devices, i);
        uint64_t devID = mt_device_get_id(dev);
        if (verbose)
            printf("  [%ld] device ID: %llu (0x%llx)\n", (long)i,
                   (unsigned long long)devID, (unsigned long long)devID);

        CFTypeRef act = pMTActuatorCreateFromDeviceID(devID);
        if (act) {
            IOReturn ret = pMTActuatorOpen(act, 0);
            if (ret == kIOReturnSuccess) {
                if (verbose)
                    printf("       ^ has haptic actuator\n");
                pMTActuatorClose(act);
                if (found_id == -1)
                    found_id = (int64_t)devID;
            } else if (verbose) {
                printf("       ^ actuator open failed (0x%x)\n", ret);
            }
            CFRelease(act);
        }
    }
    CFRelease(devices);
    return found_id;
}

// --- Listen mode (-f) ---

static const char *state_name(int state) {
    switch (state) {
    case 0:  return "none";
    case 1:  return "start";
    case 2:  return "hover";
    case 3:  return "make";
    case 4:  return "touch";
    case 5:  return "press";
    case 6:  return "tap";
    case 7:  return "lift";
    default: return "?";
    }
}

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void touch_callback(void *device, MTTouch *touches, int nFingers,
                            double timestamp, int frame) {
    (void)device;
    if (nFingers <= 0) return;

    printf("frame %-6d  t=%.4f  fingers=%d\n", frame, timestamp, nFingers);
    for (int i = 0; i < nFingers; i++) {
        MTTouch *t = &touches[i];
        printf("  [%d] state=%-5s  pos=(%.3f, %.3f)  vel=(%.3f, %.3f)  "
               "pressure=%.1f  size=%.3f  angle=%.2f  "
               "axis=(%.2f, %.2f)  density=%.2f  "
               "abs_x=%.1fmm  abs_vel=(%.1f, %.1f)mm/s  "
               "zPressure=%.3f\n",
               t->pathIndex, state_name(t->state),
               t->norm_x, t->norm_y,
               t->vel_x, t->vel_y,
               t->pressure, t->size, t->angle,
               t->majorAxis, t->minorAxis, t->density,
               t->abs_x, t->abs_vel_x, t->abs_vel_y,
               t->zPressure);
    }
}

static int run_listen_mode(void) {
    if (!pMTDeviceCreateList || !pMTRegisterContactFrameCallback ||
        !pMTDeviceStart || !pMTDeviceStop) {
        fprintf(stderr, "error: missing symbols for listen mode\n");
        return 1;
    }

    CFMutableArrayRef devices = pMTDeviceCreateList();
    if (!devices || CFArrayGetCount(devices) == 0) {
        fprintf(stderr, "error: no multitouch devices found\n");
        return 1;
    }

    void *dev = (void *)CFArrayGetValueAtIndex(devices, 0);
    uint64_t devID = mt_device_get_id(dev);
    printf("Listening on device %llu — touch the trackpad (ctrl-c to stop)\n\n",
           (unsigned long long)devID);

    signal(SIGINT, sigint_handler);

    pMTRegisterContactFrameCallback(dev, touch_callback);
    pMTDeviceStart(dev, 0);

    while (g_running) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    }

    pMTDeviceStop(dev);
    CFRelease(devices);
    printf("\nStopped.\n");
    return 0;
}

// --- Main ---

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -w <id>       Waveform ID to actuate (default: 2)\n"
        "                  1 = weak click\n"
        "                  2 = strong click (Force Touch feel)\n"
        "                  3 = buzz / notification\n"
        "                  4 = light tap\n"
        "                  5 = medium tap\n"
        "                  6 = strong tap\n"
        "  -d <deviceID> Multitouch device ID (default: auto-detect)\n"
        "  -r <count>    Repeat count (default: 1)\n"
        "  -i <ms>       Interval between repeats in milliseconds (default: 200)\n"
        "  -f            Listen: stream live touch data (position, pressure, etc.)\n"
        "  -l            List: actuate waveforms 1-20 with a pause between each\n"
        "  -s            Scan: list multitouch devices and exit\n"
        "  -h            Show this help\n"
        "\n"
        "Examples:\n"
        "  %s -s              # scan for devices\n"
        "  %s -w 2            # single strong click\n"
        "  %s -w 3 -r 3       # buzz three times\n"
        "  %s -l              # cycle through waveforms 1-20\n"
        "  %s -f              # stream live touch data\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int32_t waveform = 2;
    int64_t deviceID = -1;
    int repeat = 1;
    int interval_ms = 200;
    int list_mode = 0;
    int scan_mode = 0;
    int listen_mode = 0;
    int opt;

    while ((opt = getopt(argc, argv, "w:d:r:i:flsh")) != -1) {
        switch (opt) {
        case 'w': waveform = atoi(optarg); break;
        case 'd': deviceID = strtoll(optarg, NULL, 0); break;
        case 'r': repeat = atoi(optarg); break;
        case 'i': interval_ms = atoi(optarg); break;
        case 'f': listen_mode = 1; break;
        case 'l': list_mode = 1; break;
        case 's': scan_mode = 1; break;
        case 'h':
        default:  usage(argv[0]); return (opt == 'h') ? 0 : 1;
        }
    }

    if (load_mt() != 0)
        return 1;

    if (scan_mode) {
        printf("Scanning for haptic devices...\n");
        find_trackpad_device_id(1);
        return 0;
    }

    if (listen_mode)
        return run_listen_mode();

    // Auto-detect device if not specified
    if (deviceID == -1) {
        deviceID = find_trackpad_device_id(0);
        if (deviceID == -1) {
            fprintf(stderr, "error: no haptic-capable device found\n");
            return 1;
        }
        printf("Using device ID: %lld\n\n", deviceID);
    }

    CFTypeRef actuator = pMTActuatorCreateFromDeviceID((uint64_t)deviceID);
    if (!actuator) {
        fprintf(stderr, "error: could not create actuator for device %lld\n", deviceID);
        return 1;
    }

    IOReturn ret = pMTActuatorOpen(actuator, 0);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "error: MTActuatorOpen failed (0x%x)\n", ret);
        CFRelease(actuator);
        return 1;
    }

    if (list_mode) {
        printf("Cycling through waveforms 1-20 (500ms apart)...\n");
        for (int32_t w = 1; w <= 20; w++) {
            printf("  waveform %2d: ", w);
            fflush(stdout);
            ret = pMTActuatorActuate(actuator, w, 0, 0, 0);
            if (ret == kIOReturnSuccess)
                printf("ok\n");
            else
                printf("failed (0x%x)\n", ret);
            usleep(500 * 1000);
        }
    } else {
        for (int i = 0; i < repeat; i++) {
            ret = pMTActuatorActuate(actuator, waveform, 0, 0, 0);
            if (ret != kIOReturnSuccess) {
                fprintf(stderr, "error: MTActuatorActuate(%d) failed (0x%x)\n", waveform, ret);
                break;
            }
            if (i < repeat - 1)
                usleep(interval_ms * 1000);
        }
        if (ret == kIOReturnSuccess)
            printf("Actuated waveform %d x%d\n", waveform, repeat);
    }

    pMTActuatorClose(actuator);
    CFRelease(actuator);
    return (ret == kIOReturnSuccess) ? 0 : 1;
}
