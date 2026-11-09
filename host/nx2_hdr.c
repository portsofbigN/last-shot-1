// HDR output. The game renders HDR10 itself (PQ-encoded BT.2020 in an RGB10A2 frame) when nn::vi says the display is
// in HDR (QueryDisplayHdrStatus, nx2_gfx.c); the port then presents that frame unchanged on an HDR10 swapchain
// (nx2_vk.c). NX2_HDR=1 asks for it; it is used only while Windows has HDR on for a monitor ("Use HDR" in the display
// settings), since an SDR desktop cannot show the PQ frame. The FPS overlay, drawn in SDR, is converted here.
#define COBJMACROS
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dxgi1_6.h>

#include "host.h"

static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID iid_output6 = {0x068346e8, 0xaaec, 0x4b84, {0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1}};

static int state = -1;      // -1 not decided, 0 SDR, 1 HDR
static uint16_t peak = 1000;

// 1 when a monitor is in Windows HDR (its colour space is PQ / BT.2020); *nits its peak brightness
static int windows_hdr(float *nits) {
    HMODULE m = LoadLibraryA("dxgi.dll");
    if (!m) return 0;
    typedef HRESULT(WINAPI * Create)(REFIID, void **);
    Create create = (Create)(void *)GetProcAddress(m, "CreateDXGIFactory1");
    IDXGIFactory1 *f = NULL;
    if (!create || FAILED(create(&iid_factory1, (void **)&f))) return 0;
    int found = 0;
    IDXGIAdapter1 *a;
    for (UINT i = 0; !found && IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        IDXGIOutput *o;
        for (UINT k = 0; !found && IDXGIAdapter1_EnumOutputs(a, k, &o) == S_OK; k++) {
            IDXGIOutput6 *o6;
            if (SUCCEEDED(IDXGIOutput_QueryInterface(o, &iid_output6, (void **)&o6))) {
                DXGI_OUTPUT_DESC1 d;
                if (SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d)) && d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                    found = 1;
                    *nits = d.MaxLuminance;
                }
                IDXGIOutput6_Release(o6);
            }
            IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    return found;
}

int nx2_hdr_wanted(void) {
    if (state >= 0) return state;
    const char *e = getenv("NX2_HDR");
    state = 0;
    if (!e || strcmp(e, "1")) return 0;
    float nits = 0;
    if (getenv("NX2_HDR_FORCE")) {   // tests: without an HDR monitor
        state = 1;
    } else if (windows_hdr(&nits)) {
        state = 1;
        if (nits >= 100 && nits < 10000) peak = (uint16_t)nits;
    } else {
        rt_log("hdr: asked for, but Windows has HDR off on every monitor: SDR");
        return 0;
    }
    rt_log("hdr: on (peak %u nits)", peak);
    return 1;
}
uint16_t nx2_hdr_peak(void) { return peak; }

static double pq_encode(double nits) {   // SMPTE ST 2084
    const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    double y = pow(nits / 10000.0, m1);
    return pow((c1 + c2 * y) / (1 + c3 * y), m2);
}
static double pq_decode(double e) {
    const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    double p = pow(e, 1 / m2), n = p - c1 > 0 ? p - c1 : 0;
    return 10000.0 * pow(n / (c2 - c3 * p), 1 / m1);
}
static double srgb_to_linear(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }

// The overlay's B8G8R8A8 pixels (SDR, sRGB) to A2B10G10R10 PQ at the brightness of SDR white (203 nits)
void nx2_hdr_overlay(uint32_t *px, size_t n) {
    static uint16_t lut[256];
    if (!lut[255])
        for (int i = 0; i < 256; i++) {   // at the game's white (NX2_HDR_WHITE, nx2_gfx.c)
            double white = getenv("NX2_HDR_WHITE") ? atof(getenv("NX2_HDR_WHITE")) : 203.0;
            white = white < 80 ? 80 : white > 500 ? 500 : white;
            lut[i] = (uint16_t)lround(pq_encode(srgb_to_linear(i / 255.0) * white) * 1023.0);
        }
    for (size_t i = 0; i < n; i++) {
        uint32_t v = px[i], b = v & 255, g = v >> 8 & 255, r = v >> 16 & 255, a = v >> 24;
        px[i] = lut[r] | (uint32_t)lut[g] << 10 | (uint32_t)lut[b] << 20 | (a >= 128 ? 3u : a >= 64 ? 2u : a ? 1u : 0u) << 30;
    }
}

// Tests: a PQ RGB10A2 frame (R in the low bits) to sRGB RGBA8 in place, SDR white at 203 nits, clipped
void nx2_hdr_to_srgb8(uint8_t *p, size_t n) {
    static float lin[1024];   // PQ code -> linear, SDR white 1
    static uint8_t enc[4096];   // linear 0..1 -> sRGB 8-bit
    static int ready;
    if (!ready) {
        for (int i = 0; i < 1024; i++) lin[i] = (float)(pq_decode(i / 1023.0) / 203.0);
        for (int i = 0; i < 4096; i++) {
            double l = i / 4095.0;
            enc[i] = (uint8_t)lround((l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055) * 255.0);
        }
        ready = 1;
    }
    uint32_t mx = 0, mn = 1023;
    size_t over = 0, hist[1024] = {0};
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t v0;
        memcpy(&v0, p + i * 4, 4);
        uint32_t c = v0 & 1023, c1 = v0 >> 10 & 1023, c2 = v0 >> 20 & 1023;
        c = c > c1 ? c : c1, c = c > c2 ? c : c2;
        mx = c > mx ? c : mx, mn = c < mn ? c : mn;
        over += lin[c] > 1.0f;
        hist[c]++, sum += lin[c] * 203.0;
    }
    size_t acc = 0;
    uint32_t p1 = 0, p50 = 0;
    for (uint32_t c = 0; c < 1024; c++) {
        acc += hist[c];
        if (!p1 && acc >= n / 100) p1 = c ? c : 1;
        if (!p50 && acc >= n / 2) p50 = c ? c : 1;
    }
    rt_log("hdr: shot: brightest %.0f nits, darkest %.3f nits (1%% darker than %.2f), median %.0f, mean %.0f; %.1f%% above "
           "SDR white", lin[mx] * 203.0, lin[mn] * 203.0, lin[p1] * 203.0, lin[p50] * 203.0, sum / (double)n,
           100.0 * over / (double)n);
    for (size_t i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, p + i * 4, 4);
        float r = lin[v & 1023], g = lin[v >> 10 & 1023], b = lin[v >> 20 & 1023];
        float o[3] = {1.6605f * r - 0.5876f * g - 0.0728f * b, -0.1246f * r + 1.1329f * g - 0.0083f * b,
                      -0.0182f * r - 0.1006f * g + 1.1187f * b};   // BT.2020 -> BT.709
        for (int k = 0; k < 3; k++) {
            float x = o[k] < 0 ? 0 : o[k] > 1 ? 1 : o[k];
            p[i * 4 + k] = enc[(int)(x * 4095.0f + 0.5f)];
        }
        p[i * 4 + 3] = 255;
    }
}
