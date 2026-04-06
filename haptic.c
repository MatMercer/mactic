#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <dlfcn.h>

// MultitouchSupport private framework — loaded dynamically to get correct
// calling conventions on arm64e (pointer authentication can break extern decls).

#define MT_FW "/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport"

// Offset of the device ID field inside the opaque MTDevice struct.
// Found empirically on M3 MacBook — may change across macOS versions.
#define MTDEVICE_ID_OFFSET 64

// M3 MacBook Pro trackpad physical dimensions
#define TRACKPAD_W_MM 155.0f
#define TRACKPAD_H_MM 99.0f

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

// Function pointer types
typedef CFTypeRef         (*MTActuatorCreateFromDeviceID_t)(uint64_t deviceID);
typedef IOReturn          (*MTActuatorOpen_t)(CFTypeRef actuator, uint32_t options);
typedef IOReturn          (*MTActuatorClose_t)(CFTypeRef actuator);
typedef IOReturn          (*MTActuatorActuate_t)(CFTypeRef actuator, int32_t waveform, uint32_t a1, uint32_t a2, uint32_t a3);
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
    pMTActuatorCreateFromDeviceID   = dlsym(mt_handle, "MTActuatorCreateFromDeviceID");
    pMTActuatorOpen                 = dlsym(mt_handle, "MTActuatorOpen");
    pMTActuatorClose                = dlsym(mt_handle, "MTActuatorClose");
    pMTActuatorActuate              = dlsym(mt_handle, "MTActuatorActuate");
    pMTDeviceCreateList             = dlsym(mt_handle, "MTDeviceCreateList");
    pMTRegisterContactFrameCallback = dlsym(mt_handle, "MTRegisterContactFrameCallback");
    pMTDeviceStart                  = dlsym(mt_handle, "MTDeviceStart");
    pMTDeviceStop                   = dlsym(mt_handle, "MTDeviceStop");

    if (!pMTActuatorCreateFromDeviceID || !pMTActuatorOpen ||
        !pMTActuatorClose || !pMTActuatorActuate) {
        fprintf(stderr, "error: could not resolve MTActuator symbols\n");
        return -1;
    }
    if (!pMTDeviceCreateList)
        fprintf(stderr, "warning: could not resolve MTDeviceCreateList\n");
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
                if (verbose) printf("       ^ has haptic actuator\n");
                pMTActuatorClose(act);
                if (found_id == -1) found_id = (int64_t)devID;
            } else if (verbose) {
                printf("       ^ actuator open failed (0x%x)\n", ret);
            }
            CFRelease(act);
        }
    }
    CFRelease(devices);
    return found_id;
}

// ── Signal handling ─────────────────────────────────────────────────

static volatile sig_atomic_t g_running = 1;
static CFRunLoopRef g_runloop;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_runloop) CFRunLoopStop(g_runloop);
}

// ── Raw terminal for ESC key detection ──────────────────────────────

static struct termios g_orig_termios;
static int g_raw_mode = 0;

static void raw_mode_enable(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    g_raw_mode = 1;
}

static void raw_mode_disable(void) {
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_raw_mode = 0;
    }
}

// Called from CFRunLoop — polls stdin for ESC (0x1b) or 'q'
static void stdin_read_cb(CFFileDescriptorRef fdref, CFOptionFlags flags, void *info) {
    (void)flags; (void)info;
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 0x1b || buf[i] == 'q') {  // ESC or q
            g_running = 0;
            if (g_runloop) CFRunLoopStop(g_runloop);
            return;
        }
    }
    // Re-enable callback for next read
    CFFileDescriptorEnableCallBacks(fdref, kCFFileDescriptorReadCallBack);
}

static CFFileDescriptorRef stdin_source_setup(void) {
    if (!isatty(STDIN_FILENO)) return NULL;
    CFFileDescriptorRef fdref = CFFileDescriptorCreate(
        NULL, STDIN_FILENO, false, stdin_read_cb, NULL);
    CFFileDescriptorEnableCallBacks(fdref, kCFFileDescriptorReadCallBack);
    CFRunLoopSourceRef src = CFFileDescriptorCreateRunLoopSource(NULL, fdref, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
    CFRelease(src);
    return fdref;
}

// Suppress "*** Recognized" noise the framework prints to stdout/stderr
static void mt_device_start_quiet(void *dev) {
    fflush(stdout); fflush(stderr);
    int so = dup(STDOUT_FILENO), se = dup(STDERR_FILENO);
    int nul = open("/dev/null", O_WRONLY);
    dup2(nul, STDOUT_FILENO); dup2(nul, STDERR_FILENO); close(nul);
    pMTDeviceStart(dev, 0);
    fflush(stdout); fflush(stderr);
    dup2(so, STDOUT_FILENO); dup2(se, STDERR_FILENO);
    close(so); close(se);
}

// ── Listen mode (-f) ────────────────────────────────────────────────

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
               t->norm_x, t->norm_y, t->vel_x, t->vel_y,
               t->pressure, t->size, t->angle,
               t->majorAxis, t->minorAxis, t->density,
               t->abs_x, t->abs_vel_x, t->abs_vel_y, t->zPressure);
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
    printf("Listening on device %llu — touch the trackpad (esc/q/ctrl-c to stop)\n\n",
           (unsigned long long)devID);
    signal(SIGINT, sigint_handler);
    g_runloop = CFRunLoopGetCurrent();
    raw_mode_enable();
    CFFileDescriptorRef fdref = stdin_source_setup();
    pMTRegisterContactFrameCallback(dev, touch_callback);
    mt_device_start_quiet(dev);
    while (g_running)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
    pMTDeviceStop(dev);
    CFRelease(devices);
    if (fdref) CFRelease(fdref);
    raw_mode_disable();
    printf("\nStopped.\n");
    return 0;
}

// ── ASCII pressure viewer (-a) ──────────────────────────────────────

#define MAX_CW 140
#define MAX_CH 60
#define MAX_FINGERS 11

static int   a_cw, a_ch, a_dw, a_dh;
static float a_heat[MAX_CH * 4][MAX_CW * 2];
static float a_pmax = 1.4f;  // size field: ~0.3 (light) to ~1.35 (hard press)

static MTTouch a_touches[MAX_FINGERS];
static int     a_nfingers;
static int     a_first = 1;

// Flicker-free output buffer
#define OB_SZ (1 << 18)
static char ob[OB_SZ];
static int  ob_n;

static void ob_s(const char *s) {
    int n = (int)strlen(s);
    if (ob_n + n < OB_SZ) { memcpy(ob + ob_n, s, n); ob_n += n; }
}

__attribute__((format(printf, 1, 2)))
static void ob_f(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ob + ob_n, OB_SZ - ob_n, fmt, ap);
    va_end(ap);
    if (n > 0 && ob_n + n < OB_SZ) ob_n += n;
}

static void ob_flush(void) { write(STDOUT_FILENO, ob, ob_n); ob_n = 0; }

// Braille: U+2800 + bit pattern
//   col0: row0=bit0  row1=bit1  row2=bit2  row3=bit6
//   col1: row0=bit3  row1=bit4  row2=bit5  row3=bit7
static const int BR[4][2] = {{0,3},{1,4},{2,5},{6,7}};

// Heat colormap: deep blue → blue → cyan → green → yellow → red → white
static void hcol(float t, int *r, int *g, int *b) {
    if (t <= 0.0f)       { *r = 0;   *g = 0;   *b = 0;   return; }
    if (t > 1.0f) t = 1.0f;
    if (t < 0.12f)       { float s = t/0.12f;           *r = 0;                  *g = 0;                  *b = 80+(int)(s*120);     }
    else if (t < 0.25f)  { float s = (t-0.12f)/0.13f;   *r = 0;                  *g = (int)(s*180);       *b = 200+(int)(s*55);     }
    else if (t < 0.42f)  { float s = (t-0.25f)/0.17f;   *r = 0;                  *g = 180+(int)(s*75);    *b = (int)((1-s)*255);    }
    else if (t < 0.60f)  { float s = (t-0.42f)/0.18f;   *r = (int)(s*255);       *g = 255;                *b = 0;                   }
    else if (t < 0.80f)  { float s = (t-0.60f)/0.20f;   *r = 255;                *g = (int)((1-s)*255);   *b = 0;                   }
    else                 { float s = (t-0.80f)/0.20f;   *r = 255;                *g = (int)(s*255);       *b = (int)(s*255);        }
}

static void ascii_init(void) {
    struct winsize ws = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int tw = ws.ws_col > 20 ? ws.ws_col : 80;
    int th = ws.ws_row > 10 ? ws.ws_row : 40;

    a_cw = tw - 4;
    if (a_cw > MAX_CW) a_cw = MAX_CW;

    // Braille dots are visually square (char cells are ~1:2, braille is 2x4).
    // canvas_h = canvas_w × (H_mm / W_mm) / 2
    a_ch = (int)(a_cw * (TRACKPAD_H_MM / TRACKPAD_W_MM) / 2.0f + 0.5f);
    if (a_ch > MAX_CH) a_ch = MAX_CH;

    // Respect terminal height (need canvas + 4 lines for border/status)
    int max_h = th - 4;
    if (a_ch > max_h) {
        a_ch = max_h > 4 ? max_h : 4;
        a_cw = (int)(a_ch * 2.0f * (TRACKPAD_W_MM / TRACKPAD_H_MM) + 0.5f);
        if (a_cw > MAX_CW) a_cw = MAX_CW;
    }

    a_dw = a_cw * 2;
    a_dh = a_ch * 4;
    memset(a_heat, 0, sizeof(a_heat));
}

static void ascii_paint(void) {
    float mpdx = TRACKPAD_W_MM / a_dw;
    float mpdy = TRACKPAD_H_MM / a_dh;

    for (int f = 0; f < a_nfingers; f++) {
        MTTouch *t = &a_touches[f];
        if (t->state <= 0) continue;

        float cx = t->norm_x * a_dw;
        float cy = (1.0f - t->norm_y) * a_dh;

        float sa = (t->majorAxis / 2.0f) / mpdx * 1.3f;
        float sb = (t->minorAxis / 2.0f) / mpdy * 1.3f;
        if (sa < 2.0f) sa = 2.0f;
        if (sb < 2.0f) sb = 2.0f;

        float ca = cosf(-t->angle), sn = sinf(-t->angle);

        float p = t->size;  // contact area: ~0.3 (light) → ~1.35 (hard)
        if (p > a_pmax) a_pmax = p;
        float pn = p / a_pmax;
        if (pn < 0.15f) pn = 0.15f;  // minimum visibility

        float rmax = (sa > sb ? sa : sb) * 2.0f;
        int x0 = (int)(cx - rmax); if (x0 < 0) x0 = 0;
        int x1 = (int)(cx + rmax); if (x1 >= a_dw) x1 = a_dw - 1;
        int y0 = (int)(cy - rmax); if (y0 < 0) y0 = 0;
        int y1 = (int)(cy + rmax); if (y1 >= a_dh) y1 = a_dh - 1;

        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float dx = x - cx, dy = y - cy;
                float rx = dx * ca - dy * sn;
                float ry = dx * sn + dy * ca;
                float d2 = (rx*rx)/(sa*sa+0.01f) + (ry*ry)/(sb*sb+0.01f);
                float val = pn * expf(-d2 * 1.5f);
                if (val > a_heat[y][x])
                    a_heat[y][x] = val;
            }
        }
    }
}

static void ascii_render(void) {
    ob_n = 0;

    ob_s("\033[H");
    if (a_first) {
        ob_s("\033[2J\033[?25l");
        a_first = 0;
    }

    // Header
    ob_f(" \033[1mhaptic\033[22m \033[38;5;240m│\033[0m %d×%dmm "
         "\033[38;5;240m│\033[0m esc/q to quit\033[K\n",
         (int)TRACKPAD_W_MM, (int)TRACKPAD_H_MM);

    // Top border
    ob_s("\033[38;5;240m╭");
    for (int i = 0; i < a_cw; i++) ob_s("─");
    ob_s("╮\033[0m\n");

    // Canvas
    for (int row = 0; row < a_ch; row++) {
        ob_s("\033[38;5;240m│\033[0m");
        int pr = -1, pg = -1, pb = -1;
        for (int col = 0; col < a_cw; col++) {
            int dr = row * 4, dc = col * 2;
            uint32_t cp = 0x2800;
            float maxp = 0;
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 2; c++) {
                    int dy = dr + r, dx = dc + c;
                    if (dy < a_dh && dx < a_dw) {
                        float p = a_heat[dy][dx];
                        if (p > 0.02f) {
                            cp |= (1 << BR[r][c]);
                            if (p > maxp) maxp = p;
                        }
                    }
                }
            if (cp != 0x2800) {
                int r, g, b;
                hcol(maxp, &r, &g, &b);
                if (r != pr || g != pg || b != pb) {
                    ob_f("\033[38;2;%d;%d;%dm", r, g, b);
                    pr = r; pg = g; pb = b;
                }
                char u[4] = {
                    (char)(0xE0 | (cp >> 12)),
                    (char)(0x80 | ((cp >> 6) & 0x3F)),
                    (char)(0x80 | (cp & 0x3F)), 0
                };
                ob_s(u);
            } else {
                if (pr != -1) { ob_s("\033[0m"); pr = -1; pg = -1; pb = -1; }
                ob_s(" ");
            }
        }
        ob_s("\033[0m\033[38;5;240m│\033[0m\n");
    }

    // Bottom border
    ob_s("\033[38;5;240m╰");
    for (int i = 0; i < a_cw; i++) ob_s("─");
    ob_s("╯\033[0m\n");

    // Status: finger count + pressure bar
    int n = a_nfingers;
    float maxpres = 0;
    for (int f = 0; f < n; f++)
        if (a_touches[f].size > maxpres)
            maxpres = a_touches[f].size;

    float pnorm = a_pmax > 0 ? maxpres / a_pmax : 0;
    if (pnorm > 1.0f) pnorm = 1.0f;
    int bw = 24;
    int filled = (int)(pnorm * bw + 0.5f);

    ob_f(" %d finger%-2s ", n, n == 1 ? " " : "s");
    for (int i = 0; i < bw; i++) {
        if (i < filled) {
            int r, g, b;
            hcol((float)(i + 1) / bw, &r, &g, &b);
            ob_f("\033[38;2;%d;%d;%dm█", r, g, b);
        } else {
            ob_s("\033[38;5;236m░");
        }
    }
    ob_f("\033[0m %.2f", maxpres);

    // Per-finger positions
    if (n > 0 && n <= 5) {
        ob_s("  ");
        for (int f = 0; f < n; f++)
            ob_f(" \033[38;5;245m(%+.2f,%+.2f)\033[0m",
                 a_touches[f].norm_x, a_touches[f].norm_y);
    }

    ob_s("\033[K\033[J");
    ob_flush();
}

static void ascii_touch_cb(void *device, MTTouch *touches, int nFingers,
                            double timestamp, int frame) {
    (void)device; (void)timestamp; (void)frame;
    a_nfingers = nFingers > MAX_FINGERS ? MAX_FINGERS : nFingers;
    if (nFingers > 0)
        memcpy(a_touches, touches, a_nfingers * sizeof(MTTouch));
}

static void ascii_timer_cb(CFRunLoopTimerRef timer, void *info) {
    (void)timer; (void)info;

    // Decay heat
    for (int y = 0; y < a_dh; y++)
        for (int x = 0; x < a_dw; x++)
            a_heat[y][x] *= 0.05f;  // ~8ms decay at 30fps — near-instant fade

    // Decay a_pmax back toward baseline so it recovers after palm events
    a_pmax = a_pmax * 0.97f + 1.4f * 0.03f;

    ascii_paint();
    ascii_render();
}

static int run_ascii_mode(void) {
    if (!pMTDeviceCreateList || !pMTRegisterContactFrameCallback ||
        !pMTDeviceStart || !pMTDeviceStop) {
        fprintf(stderr, "error: missing symbols for ascii mode\n");
        return 1;
    }
    CFMutableArrayRef devices = pMTDeviceCreateList();
    if (!devices || CFArrayGetCount(devices) == 0) {
        fprintf(stderr, "error: no multitouch devices found\n");
        return 1;
    }
    void *dev = (void *)CFArrayGetValueAtIndex(devices, 0);

    ascii_init();

    signal(SIGINT, sigint_handler);
    g_runloop = CFRunLoopGetCurrent();
    raw_mode_enable();
    CFFileDescriptorRef fdref = stdin_source_setup();

    // Alternate screen buffer
    printf("\033[?1049h\033[?25l");
    fflush(stdout);

    pMTRegisterContactFrameCallback(dev, ascii_touch_cb);
    mt_device_start_quiet(dev);

    // 30 fps render timer
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
        NULL, CFAbsoluteTimeGetCurrent(), 1.0 / 30.0, 0, 0,
        ascii_timer_cb, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);

    while (g_running)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);

    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    CFRelease(timer);
    pMTDeviceStop(dev);
    CFRelease(devices);
    if (fdref) CFRelease(fdref);

    // Restore terminal
    raw_mode_disable();
    printf("\033[?25h\033[?1049l");
    fflush(stdout);
    return 0;
}

// ── Main ────────────────────────────────────────────────────────────

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -a            ASCII pressure viewer (braille heatmap)\n"
        "  -f            Listen: stream live touch data (position, pressure, etc.)\n"
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
        "  %s -a              # ascii pressure heatmap\n"
        "  %s -f              # stream live touch data\n"
        "  %s -w 2            # single strong click\n"
        "  %s -w 3 -r 3       # buzz three times\n"
        "  %s -l              # cycle through waveforms 1-20\n"
        "  %s -s              # scan for devices\n",
        prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int32_t waveform = 2;
    int64_t deviceID = -1;
    int repeat = 1;
    int interval_ms = 200;
    int list_mode = 0;
    int scan_mode = 0;
    int listen_mode = 0;
    int ascii_mode = 0;
    int opt;

    while ((opt = getopt(argc, argv, "w:d:r:i:aflsh")) != -1) {
        switch (opt) {
        case 'w': waveform = atoi(optarg); break;
        case 'd': deviceID = strtoll(optarg, NULL, 0); break;
        case 'r': repeat = atoi(optarg); break;
        case 'i': interval_ms = atoi(optarg); break;
        case 'a': ascii_mode = 1; break;
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

    if (ascii_mode)
        return run_ascii_mode();

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
