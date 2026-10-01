// Chess analyzer root overlay (ARM64). UI thread: ImGui + EGL + touch. Worker thread: screencap,
// board detection, recognition, Stockfish. All communication goes through Shared.
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <sys/stat.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "a_native_window_creator.h"
#include "vision.h"
#include "chess_core.h"
#include "stockfish.h"
#include "settings.h"
#include "touch_input.h"
#include "log.h"

static EGLDisplay gd = EGL_NO_DISPLAY;
static EGLSurface gs = EGL_NO_SURFACE;
static EGLContext gc = EGL_NO_CONTEXT;
static ANativeWindow* gw = nullptr;
static const char* STATE = "/data/adb/chess_analyzer/run/analysis.json";
static const char* LEARN = "/data/adb/chess_analyzer/run/templates.bin";

static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_reqScan{false}, g_reqReanalyze{false}, g_reqLearn{false};
static void onSignal(int) { g_quit = true; }

static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ shared state
struct Shared {
    std::mutex m;
    Settings cfg;                    // UI -> worker
    // worker -> UI
    vision::BoardDetect bd; bool bdValid = false; bool wb = true; int imgW = 0, imgH = 0;
    uint64_t lastGoodMs = 0;
    std::string fen, bm, ev, pv, sfStatus = "STOCKFISH: STARTING", scanStatus = "WAITING FOR FIRST SCAN";
    int evalDepth = 0, scans = 0, analyses = 0;
    bool learned = false;
};
static Shared S;
static const uint64_t HOLD_MS = 6000;   // keep last good board/FEN/arrow this long when recognition fails

// ------------------------------------------------------------------ helpers
static bool root() { return geteuid() == 0; }
static bool mkdirs() {
    mkdir("/data/adb/chess_analyzer", 0755);
    mkdir("/data/adb/chess_analyzer/run", 0755);
    return access("/data/adb/chess_analyzer/run", W_OK) == 0;
}
static bool capture(vision::Image& im, std::string& err) {
    FILE* p = popen("/system/bin/screencap", "r");
    if (!p) { err = "screencap unavailable"; return false; }
    std::vector<uint8_t> b; uint8_t x[65536]; size_t n;
    while ((n = fread(x, 1, sizeof x, p)) > 0) b.insert(b.end(), x, x + n);
    int rc = pclose(p);
    if (rc != 0) { err = "screencap exited with status " + std::to_string(rc); return false; }
    return vision::parseRawScreencap(b, im, err);
}
// log only when the message changes (never per-frame / per-scan spam)
static void logChange(std::string& last, const std::string& msg) { if (msg != last) { LOG("%s", msg.c_str()); last = msg; } }

static void writeState(const vision::BoardDetect& bd, const std::string& fen, const std::string& bm,
                       const std::string& ev, int depth, bool ok, const std::string& why) {
    std::ofstream f(STATE);
    std::string w = why; for (char& c : w) if (c == '"') c = '\'';
    f << "{\n  \"ok\": " << (ok ? "true" : "false") << ",\n  \"fen\": \"" << fen << "\",\n  \"bestmove\": \"" << bm
      << "\",\n  \"evaluation\": \"" << ev << "\",\n  \"depth\": " << depth << ",\n  \"board_x\": " << bd.x
      << ", \"board_y\": " << bd.y << ", \"board_w\": " << bd.size << ", \"board_h\": " << bd.size
      << ",\n  \"reason\": \"" << w << "\"\n}\n";
}

// ------------------------------------------------------------------ scanning
struct ScanOut {
    bool ok = false; vision::BoardDetect bd; bool bdFound = false; bool wb = true;
    std::string fen, why; int imgW = 0, imgH = 0;
};
struct ScanCtx {
    vision::Recognizer rec; vision::BoardDetect lastBd; bool haveBd = false;
    std::string lastLearnLog, lastBoardLog;
};

static ScanOut scanOnce(ScanCtx& cx, const Settings& c, bool forceLearn) {
    ScanOut so; std::string err;
    vision::Image im;
    if (!capture(im, err)) { so.why = "CAPTURE FAILED: " + err; return so; }
    so.imgW = im.w; so.imgH = im.h;

    // 1) board: cheap tracking of the previous rectangle first, full tolerant detection otherwise
    vision::BoardDetect bd;
