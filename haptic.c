#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

// MultitouchSupport private framework — loaded dynamically to get correct
// calling conventions on arm64e (pointer authentication can break extern decls).

#define MT_FW "/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport"

// Offset of the device ID field inside the opaque MTDevice struct.
// Found empirically — may change across macOS versions.
#define MTDEVICE_ID_OFFSET 64

static void *mt_handle;

// Function pointer types
typedef CFTypeRef (*MTActuatorCreateFromDeviceID_t)(uint64_t deviceID);
typedef IOReturn  (*MTActuatorOpen_t)(CFTypeRef actuator, uint32_t options);
typedef IOReturn  (*MTActuatorClose_t)(CFTypeRef actuator);
typedef IOReturn  (*MTActuatorActuate_t)(CFTypeRef actuator, int32_t waveform, uint32_t a1, uint32_t a2, uint32_t a3);

typedef CFMutableArrayRef (*MTDeviceCreateList_t)(void);

static MTActuatorCreateFromDeviceID_t pMTActuatorCreateFromDeviceID;
static MTActuatorOpen_t               pMTActuatorOpen;
static MTActuatorClose_t              pMTActuatorClose;
static MTActuatorActuate_t            pMTActuatorActuate;
static MTDeviceCreateList_t           pMTDeviceCreateList;

static int load_mt(void) {
    mt_handle = dlopen(MT_FW, RTLD_LAZY);
    if (!mt_handle) {
        fprintf(stderr, "error: dlopen: %s\n", dlerror());
        return -1;
    }
    pMTActuatorCreateFromDeviceID = dlsym(mt_handle, "MTActuatorCreateFromDeviceID");
    pMTActuatorOpen               = dlsym(mt_handle, "MTActuatorOpen");
    pMTActuatorClose              = dlsym(mt_handle, "MTActuatorClose");
    pMTActuatorActuate            = dlsym(mt_handle, "MTActuatorActuate");
    pMTDeviceCreateList           = dlsym(mt_handle, "MTDeviceCreateList");

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
        "  -l            List: actuate waveforms 1-20 with a pause between each\n"
        "  -s            Scan: list multitouch devices and exit\n"
        "  -h            Show this help\n"
        "\n"
        "Examples:\n"
        "  %s -s              # scan for devices\n"
        "  %s -w 2            # single strong click\n"
        "  %s -w 3 -r 3       # buzz three times\n"
        "  %s -l              # cycle through waveforms 1-20\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int32_t waveform = 2;
    int64_t deviceID = -1;
    int repeat = 1;
    int interval_ms = 200;
    int list_mode = 0;
    int scan_mode = 0;
    int opt;

    while ((opt = getopt(argc, argv, "w:d:r:i:lsh")) != -1) {
        switch (opt) {
        case 'w': waveform = atoi(optarg); break;
        case 'd': deviceID = strtoll(optarg, NULL, 0); break;
        case 'r': repeat = atoi(optarg); break;
        case 'i': interval_ms = atoi(optarg); break;
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
