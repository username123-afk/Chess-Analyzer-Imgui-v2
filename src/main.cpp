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
    std::string turn = "--";
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
// A FLAG_SECURE window is captured by screencap as solid black. Detect that so the log/UI say so
// instead of a misleading "board not found".
static bool captureLooksBlank(const vision::Image& im) {
    if (im.w < 16 || im.h < 16) return false;
    int bright = 0, n = 0;
    for (int y = 0; y < im.h; y += std::max(1, im.h / 48))
        for (int x = 0; x < im.w; x += std::max(1, im.w / 48)) {
            const uint8_t* p = im.at(x, y);
            n++;
            if (p[0] > 12 || p[1] > 12 || p[2] > 12) bright++;
        }
    return bright * 100 < n;   // fewer than 1% of sampled pixels are non-black
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

// ------------------------------------------------------------------ learn diagnostics
// When template learning fails, log what each square looked like and save a picture of the
// screenshot with the detected 8x8 grid drawn in red (open it in any gallery / MT Manager).
static bool writeDebugBmp(const vision::Image& src, const vision::BoardDetect& bd, const char* path) {
    const int D = 2;
    int w = src.w / D, h = src.h / D;
    if (w < 8 || h < 8) return false;
    int rowBytes = (w * 3 + 3) & ~3;
    std::vector<uint8_t> px((size_t)rowBytes * h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* p = src.at(x * D, y * D);
            uint8_t* o = &px[(size_t)y * rowBytes + x * 3];
            o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
        }
    auto dot = [&](int x, int y) {
        for (int k = 0; k < 2; k++) {
            int xx = x + k, yy = y + k;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            uint8_t* o = &px[(size_t)yy * rowBytes + xx * 3];
            o[0] = 0; o[1] = 0; o[2] = 255;   // red (BGR)
        }
    };
    float s = bd.size / 8.f;
    for (int i = 0; i <= 8; i++) {
        for (int t = 0; t <= (int)(bd.size / D); t++) {
            dot((int)((bd.x + i * s) / D), (int)(bd.y / D) + t);
            dot((int)(bd.x / D) + t, (int)((bd.y + i * s) / D));
        }
    }
    uint8_t hdr[54] = {0};
    uint32_t fileSize = 54 + (uint32_t)px.size();
    int32_t bw = w, bh = -h;   // negative = top-down
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4);
    uint32_t off = 54, dib = 40; uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &bw, 4); memcpy(hdr + 22, &bh, 4);
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(hdr, 1, 54, f); fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    return true;
}
static void dumpLearnDebug(const vision::Image& im, const vision::BoardDetect& bd) {
    static uint64_t last = 0;
    uint64_t n = nowMs();
    if (last && n - last < 10000) return;   // at most once per 10 s
    last = n;
    float s = bd.size / 8.f;
    std::string rows;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            vision::SqFeat f = vision::analyzeSquare(im, bd.x + c * s, bd.y + r * s, s);
            rows += f.bad ? '?' : (f.empty ? '.' : 'P');
        }
        if (r < 7) rows += '/';
    }
    LOG("learn debug: board x=%.0f y=%.0f size=%.0f square=%.1f image=%dx%d", bd.x, bd.y, bd.size, s, im.w, im.h);
    LOG("learn debug: seen     (top->bottom, P=piece .=empty ?=unreadable): %s", rows.c_str());
    LOG("learn debug: expected (start position, either orientation):        PPPPPPPP/PPPPPPPP/......../......../......../......../PPPPPPPP/PPPPPPPP");
    bool ok = writeDebugBmp(im, bd, "/data/adb/chess_analyzer/run/debug_board.bmp");
    LOG("learn debug: wrote /data/adb/chess_analyzer/run/debug_board.bmp (%s) - red grid should sit exactly on the 8x8 squares", ok ? "ok" : "FAILED");
}

// ------------------------------------------------------------------ scanning
struct ScanOut {
    bool ok = false; vision::BoardDetect bd; bool bdFound = false; bool wb = true;
    chess::Board board;
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
    if (captureLooksBlank(im)) {
        so.why = "CAPTURE IS BLACK: the app on screen probably uses FLAG_SECURE (screenshots blocked)";
        return so;
    }

    // 1) board: cheap tracking of the previous rectangle first, full tolerant detection otherwise
    vision::BoardDetect bd;
    if (cx.haveBd) { bd = cx.lastBd; if (!vision::verifyBoard(im, bd)) bd = vision::BoardDetect(); }
    bool tracked = bd.found;
    if (!bd.found) bd = vision::detectBoard(im);
    if (!bd.found) { cx.haveBd = false; so.why = "BOARD NOT FOUND: " + bd.why; return so; }
    if (!tracked || std::fabs(bd.size - cx.lastBd.size) > 3 || std::fabs(bd.x - cx.lastBd.x) > 3 || std::fabs(bd.y - cx.lastBd.y) > 3) {
        char t[200];
        snprintf(t, sizeof t, "board detection: FOUND x=%.0f y=%.0f size=%.0f score=%d/%d tol=%d (image %dx%d)%s",
                 bd.x, bd.y, bd.size, bd.score, bd.maxScore, bd.tolUsed, im.w, im.h, tracked ? " [tracked]" : "");
        logChange(cx.lastBoardLog, t);
    }
    cx.lastBd = bd; cx.haveBd = true; so.bd = bd; so.bdFound = true;

    // 2) templates (learned once from the standard start position)
    if (!cx.rec.learned() || forceLearn) {
        std::string le;
        if (cx.rec.learnFromStart(im, bd.x, bd.y, bd.size, le)) {
            cx.rec.save(LEARN);
            LOG("recognition: templates learned from start position and saved to %s", LEARN);
            std::lock_guard<std::mutex> lk(S.m); S.learned = true;
        } else {
            so.why = "NEED TEMPLATES: open a game showing the START position, then LEARN (" + le + ")";
            std::string t = "recognition: template learning failed: " + le;
            logChange(cx.lastLearnLog, t);
            dumpLearnDebug(im, bd);
            if (!cx.rec.learned()) return so;
        }
    }

    // 3) piece recognition, tolerant: small board-rect jitter + looser template distance on retry
    static const float off[5][2] = {{0, 0}, {-2, 0}, {2, 0}, {0, -2}, {0, 2}};
    static const float dists[2] = {0.25f, 0.32f};
    vision::RecogResult rr, bestRr; bool gotOk = false;
    float savedDist = cx.rec.maxDist;
    for (int di = 0; di < 2 && !gotOk; di++) {
        cx.rec.maxDist = dists[di];
        for (int oi = 0; oi < 5 && !gotOk; oi++) {
            rr = cx.rec.recognize(im, bd.x + off[oi][0], bd.y + off[oi][1], bd.size);
            if (rr.ok) { gotOk = true; bestRr = rr; }
            else if (bestRr.why.empty() || rr.unknown < bestRr.unknown) bestRr = rr;
        }
    }
    cx.rec.maxDist = savedDist;
    if (!gotOk) {
        cx.haveBd = false;   // do not keep trusting a tracked rectangle that cannot be read: re-detect next scan
        so.why = "PIECE RECOGNITION FAILED: " + bestRr.why;
        return so;
    }

    // 4) orientation + validation
    int o;
    if (c.orientation < 0) {
        std::string w; o = vision::autoOrientation(bestRr.grid, w);
        if (o < 0) { so.why = "ORIENTATION UNCERTAIN: " + w; return so; }
    } else o = c.orientation;
    so.wb = (o == 1);
    chess::Board board = vision::gridToBoard(bestRr.grid, so.wb);
    std::string why;
    if (!chess::validate(board, true, false, why)) { so.why = "POSITION INVALID (not analysed): " + why; return so; }
    so.board = board;   // side to move + full legality check + FEN are done by the worker (needs history)
    so.ok = true;
    return so;
}

// ------------------------------------------------------------------ worker thread
static void worker() {
    sf::Engine eng;
    ScanCtx cx;
    if (cx.rec.load(LEARN)) LOG("recognition: templates loaded from %s", LEARN);
    else LOG("recognition: no templates yet (%s); will learn from a start-position board", LEARN);
    { std::lock_guard<std::mutex> lk(S.m); S.learned = cx.rec.learned(); }

    uint64_t nextScan = 0, engineRetryAt = 0;
    std::string candFen, analyzedFen, lastFail, lastFenLog;
    int candCount = 0, analyzedDepth = 0;
    bool wasGood = false;
    // side-to-move tracking: remembers the previous accepted position
    bool trHave = false; chess::Board trPrev; char trStm = 'w'; std::string lastTurnLog;

    auto setSf = [&](const std::string& s) { std::lock_guard<std::mutex> lk(S.m); S.sfStatus = s; };
    auto startEngine = [&](const Settings& c) -> bool {
        std::vector<std::string> cands;
        if (!c.stockfishPath.empty()) cands.push_back(c.stockfishPath);
        cands.push_back("/data/local/tmp/stockfish-android-arm64");
        cands.push_back("/data/adb/chess_analyzer/stockfish-android-arm64");
        cands.push_back("/data/adb/chess_analyzer/stockfish");
        std::string lastErr = "no candidate path";
        for (auto& p : cands) {
            std::string e;
            if (eng.start(p, e)) { setSf("STOCKFISH: READY"); return true; }
            LOG("stockfish: start failed for %s: %s", p.c_str(), e.c_str());
            lastErr = e;
        }
        setSf("STOCKFISH: ERROR: " + lastErr);
        return false;
    };
    { Settings c; { std::lock_guard<std::mutex> lk(S.m); c = S.cfg; } startEngine(c); }

    while (!g_quit) {
        Settings c; { std::lock_guard<std::mutex> lk(S.m); c = S.cfg; }
        uint64_t now = nowMs();
        bool scanNow = g_reqScan.exchange(false), reanalyze = g_reqReanalyze.exchange(false), learn = g_reqLearn.exchange(false);
        if (!c.analyzer) {
            std::lock_guard<std::mutex> lk(S.m); S.scanStatus = "ANALYZER OFF"; usleep(100000); continue;
        }
        bool due = c.autoAnalyze && now >= nextScan;
        if (!(due || scanNow || reanalyze || learn)) { usleep(25000); continue; }
        nextScan = now + (uint64_t)(c.scanInterval * 1000);
        if (reanalyze) { scanNow = true; }

        ScanOut so = scanOnce(cx, c, learn);
        now = nowMs();
        if (so.ok) {
            // ---- side to move. AUTO: whoever just moved is the side whose pieces appeared on changed squares.
            char stm; std::string how;
            if (c.sideToMove >= 0) { stm = c.sideToMove ? 'b' : 'w'; how = "manual"; }
            else if (!trHave) {
                stm = (so.board == chess::startBoard()) ? 'w' : (so.wb ? 'w' : 'b');
                how = "first position: assuming the side at the bottom";
            } else if (so.board == trPrev) { stm = trStm; how = "unchanged"; }
            else {
                int wn = 0, bn = 0;
                for (int i = 0; i < 64; i++)
                    if (so.board.sq[i] != trPrev.sq[i] && so.board.sq[i] != '.') (chess::isWhite(so.board.sq[i]) ? wn : bn)++;
                if (wn && !bn) { stm = 'b'; how = "white just moved"; }
                else if (bn && !wn) { stm = 'w'; how = "black just moved"; }
                else { stm = trStm; how = "several moves since last scan, kept"; }
            }
            std::string why;
            if (!chess::validateFull(so.board, stm, why)) {
                char alt = stm == 'w' ? 'b' : 'w'; std::string why2;
                if (c.sideToMove < 0 && chess::validateFull(so.board, alt, why2)) { stm = alt; how += "; flipped because the other side was illegal"; }
                else { so.ok = false; so.why = "POSITION INVALID (not analysed): " + why; }
            }
            if (so.ok) {
                std::string cast = chess::sanitizeCastling(so.board, c.castling);
                std::string ep = chess::sanitizeEp(so.board, stm, c.enPassant);
                so.fen = chess::fen(so.board, stm, cast, ep);
                trHave = true; trPrev = so.board; trStm = stm;
                std::string turn = std::string(stm == 'w' ? "WHITE" : "BLACK") + (c.sideToMove < 0 ? " (auto)" : " (manual)");
                { std::lock_guard<std::mutex> lk(S.m); S.turn = turn; }
                std::string tl = std::string("side to move: ") + (stm == 'w' ? "white" : "black") + " [" + how + "]";
                if (turn != lastTurnLog) { LOG("%s", tl.c_str()); lastTurnLog = turn; }
            }
        }
        {
            std::lock_guard<std::mutex> lk(S.m);
            S.scans++; S.learned = cx.rec.learned();
            if (so.imgW) { S.imgW = so.imgW; S.imgH = so.imgH; }
        }

        if (!so.ok) {
            logChange(lastFail, so.why);
            std::lock_guard<std::mutex> lk(S.m);
            bool fresh = S.lastGoodMs && (now - S.lastGoodMs) < HOLD_MS;
            if (fresh) S.scanStatus = so.why + "  [holding last position]";
            else {
                S.scanStatus = so.why;
                if (S.bdValid || !S.bm.empty()) LOG("scan: lost position for >%llus: %s", (unsigned long long)(HOLD_MS / 1000), so.why.c_str());
                S.bdValid = false; S.bm.clear(); S.ev.clear(); S.pv.clear();
            }
            if (so.bdFound && fresh) { S.bd = so.bd; }   // keep tracking the board rectangle
            wasGood = false; candCount = 0;
            continue;
        }
        lastFail.clear();

        // good frame: publish geometry; require the same FEN on consecutive scans (filters piece animations)
        {
            std::lock_guard<std::mutex> lk(S.m);
            S.bd = so.bd; S.bdValid = true; S.wb = so.wb; S.lastGoodMs = now; S.fen = so.fen;
            S.scanStatus = "BOARD OK";
        }
        if (so.fen == candFen) candCount++; else { candFen = so.fen; candCount = 1; }
        int need = (scanNow || reanalyze) ? 1 : 2;
        bool changed = so.fen != analyzedFen || c.depth != analyzedDepth;
        if (!wasGood || so.fen != lastFenLog) {
            if (so.fen != lastFenLog) LOG("recognized FEN: %s (orientation=%s)", so.fen.c_str(), so.wb ? "white-bottom" : "black-bottom");
            lastFenLog = so.fen;
        }
        wasGood = true;
        if (!(changed || reanalyze) || candCount < need) continue;

        // ---- analysis
        if (so.fen != analyzedFen) { std::lock_guard<std::mutex> lk(S.m); S.bm.clear(); S.ev.clear(); S.pv.clear(); }
        if (!eng.running()) {
            if (now < engineRetryAt && !reanalyze) { setSf("STOCKFISH: ERROR: engine down, retrying"); continue; }
            LOG("stockfish: engine not running, restarting");
            if (!startEngine(c)) { engineRetryAt = nowMs() + 3000; continue; }
        }
        setSf("STOCKFISH: ANALYZING");
        LOG("analysis start: depth=%d fen=%s", c.depth, so.fen.c_str());
        uint64_t t0 = nowMs();
        sf::Result r = eng.analyze(so.fen, so.fen.find(" b ") != std::string::npos ? 'b' : 'w', c.depth, 15000 + c.depth * 1000);
        uint64_t dt = nowMs() - t0;
        if (r.ok && !r.bestmove.empty()) {
            LOG("bestmove %s eval=%s depth=%d (%llu ms) pv=%s", r.bestmove.c_str(), r.eval.c_str(), r.depth, (unsigned long long)dt, r.pv.c_str());
            { std::lock_guard<std::mutex> lk(S.m); S.bm = r.bestmove; S.ev = r.eval; S.pv = r.pv; S.evalDepth = r.depth; S.analyses++; S.sfStatus = "STOCKFISH: BESTMOVE " + r.bestmove; }
            analyzedFen = so.fen; analyzedDepth = c.depth;
            writeState(so.bd, so.fen, r.bestmove, r.eval, r.depth, true, "");
        } else if (r.ok) {
            LOG("analysis: %s", r.err.c_str());
            { std::lock_guard<std::mutex> lk(S.m); S.sfStatus = "STOCKFISH: NO LEGAL MOVES"; }
            analyzedFen = so.fen; analyzedDepth = c.depth;
            writeState(so.bd, so.fen, "", "", 0, false, r.err);
        } else {
            LOG("analysis %s: %s (%llu ms) -> restarting engine", r.timeout ? "TIMEOUT" : "ERROR", r.err.c_str(), (unsigned long long)dt);
            setSf(r.timeout ? "STOCKFISH: TIMEOUT" : "STOCKFISH: ERROR: " + r.err);
            eng.stop();
            engineRetryAt = nowMs() + 2000;
            writeState(so.bd, so.fen, "", "", 0, false, r.err);
        }
    }
    eng.stop();
    LOG("worker: stopped");
}

// ------------------------------------------------------------------ EGL / drawing
static bool eglInit(ANativeWindow* w, int W, int H) {
    gd = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gd == EGL_NO_DISPLAY || !eglInitialize(gd, nullptr, nullptr)) return false;
    const EGLint a[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig c; EGLint n;
    if (!eglChooseConfig(gd, a, &c, 1, &n) || n != 1) return false;
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    gc = eglCreateContext(gd, c, EGL_NO_CONTEXT, ca);
    gs = eglCreateWindowSurface(gd, c, w, nullptr);
    if (gc == EGL_NO_CONTEXT || gs == EGL_NO_SURFACE || !eglMakeCurrent(gd, gs, gs, gc)) return false;
    glViewport(0, 0, W, H);
    return true;
}
static void eglEnd() {
    if (gd != EGL_NO_DISPLAY) {
        eglMakeCurrent(gd, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gs != EGL_NO_SURFACE) eglDestroySurface(gd, gs);
        if (gc != EGL_NO_CONTEXT) eglDestroyContext(gd, gc);
        eglTerminate(gd);
    }
    gd = EGL_NO_DISPLAY; gs = EGL_NO_SURFACE; gc = EGL_NO_CONTEXT;
}
static void arrow(ImDrawList* d, ImVec2 a, ImVec2 b, float w, ImU32 c) {
    ImVec2 v(b.x - a.x, b.y - a.y);
    float l = hypotf(v.x, v.y);
    if (l < 2) return;
    v.x /= l; v.y /= l;
    ImVec2 p(-v.y, v.x);
    float h = std::max(14.f, w * 2.6f);
    ImVec2 base(b.x - v.x * h * 0.9f, b.y - v.y * h * 0.9f);    // shaft stops inside the head
    d->AddLine(a, base, c, w);
    d->AddTriangleFilled(b, ImVec2(b.x - v.x * h + p.x * h * .55f, b.y - v.y * h + p.y * h * .55f),
                            ImVec2(b.x - v.x * h - p.x * h * .55f, b.y - v.y * h - p.y * h * .55f), c);
}
// square name -> centre in OVERLAY pixels. wb: white at bottom of the screen. sx/sy: screencap->overlay scale.
static ImVec2 squareCenter(const vision::BoardDetect& bd, const char* s, bool wb, float sx, float sy) {
    int f = s[0] - 'a', r = s[1] - '1';
    int col = wb ? f : 7 - f, row = wb ? 7 - r : r;
    float q = bd.size / 8.f;
    return ImVec2((bd.x + (col + .5f) * q) * sx, (bd.y + (row + .5f) * q) * sy);
}

int main() {
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, onSignal); signal(SIGINT, onSignal);
    if (!root()) { fprintf(stderr, "must run as root\n"); return 10; }
    if (!mkdirs()) { fprintf(stderr, "cannot create /data/adb/chess_analyzer/run\n"); return 11; }
    LOG("==== startup: chess-analyzer-overlay pid=%d ====", (int)getpid());

    Settings ui;
    loadSettings(ui);
    ui.clamp();
    std::string savedJson = ui.toJson();
    { std::lock_guard<std::mutex> lk(S.m); S.cfg = ui; }

    auto di = android::ANativeWindowCreator::GetDisplayInfo();
    LOG("display: %dx%d", (int)di.width, (int)di.height);
    if (di.width <= 0 || di.height <= 0) { LOG("FATAL: invalid display info"); return 12; }
    gw = android::ANativeWindowCreator::Create({.name = "ChessAnalyzerOverlay", .width = di.width, .height = di.height, .skipScreenshot = true});
    if (!gw) { LOG("FATAL: overlay window creation failed"); return 13; }
    LOG("overlay: native window created %dx%d (skipScreenshot=true)", (int)di.width, (int)di.height);
    if (!eglInit(gw, di.width, di.height)) { LOG("FATAL: EGL init failed (egl error 0x%x)", eglGetError()); return 13; }
    LOG("overlay: EGL ready (GL: %s)", (const char*)glGetString(GL_RENDERER));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = std::max(1.f, std::min((float)di.width, (float)di.height) / 540.f);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FramePadding = ImVec2(8 * scale, 7 * scale);
    ImGui::GetStyle().ItemSpacing = ImVec2(8 * scale, 8 * scale);
    ImGui::GetStyle().ScrollbarSize = 22 * scale;
    ImGui::GetStyle().GrabMinSize = 24 * scale;
    io.FontGlobalScale = scale;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Touch: raw evdev (see README/notes: no InputChannel is available without the window-creator internals).
    TouchInput touch;
    bool touchOk = touch.init(di.width, di.height, ui.touchRotation);
    LOG("input: initialised, touch devices=%d ui_scale=%.2f", touch.deviceCount(), scale);

    std::thread wt(worker);

    bool ck[4]; for (int i = 0; i < 4; i++) ck[i] = ui.castling.find("KQkq"[i]) != std::string::npos;
    int epIdx = ui.enPassant == "-" ? 0 : 1 + (ui.enPassant[0] - 'a');
    int rotIdx = ui.touchRotation < 0 ? 0 : 1 + ui.touchRotation / 90;
    uint64_t savedMsgUntil = 0, lastFrame = nowMs();
    bool saveOk = true;
    std::vector<TouchInput::Event> evs;
    bool pointerDown = false;
    // Copy the widget-backed variables (rotIdx / ck[] / epIdx) into `ui`. Must run before anything
    // reads `ui` for saving, dirty-checking or publishing to the worker.
    auto syncUi = [&]() {
        ui.touchRotation = rotIdx == 0 ? -1 : (rotIdx - 1) * 90;
        std::string c; for (int i = 0; i < 4; i++) if (ck[i]) c += "KQkq"[i];
        ui.castling = c.empty() ? "-" : c;
        ui.enPassant = epIdx == 0 ? "-" : std::string(1, (char)('a' + epIdx - 1));
    };

    while (!g_quit) {
        uint64_t now = nowMs();
        io.DeltaTime = std::max(0.001f, (now - lastFrame) / 1000.f); lastFrame = now;
        io.DisplaySize = ImVec2((float)di.width, (float)di.height);

        // ---- touch -> ImGui mouse
        evs.clear(); touch.setRotation(ui.touchRotation); touch.poll(evs);
        for (auto& e : evs) {
#if IMGUI_VERSION_NUM >= 18700
            if (e.type == TouchInput::Event::Down) { io.AddMousePosEvent(e.x, e.y); io.AddMouseButtonEvent(0, true); }
            else if (e.type == TouchInput::Event::Move) io.AddMousePosEvent(e.x, e.y);
            else { io.AddMousePosEvent(e.x, e.y); io.AddMouseButtonEvent(0, false); }
#else
            io.MousePos = ImVec2(e.x, e.y); io.MouseDown[0] = e.type != TouchInput::Event::Up;
#endif
            pointerDown = e.type != TouchInput::Event::Up;
        }
        if (!pointerDown) {
#if IMGUI_VERSION_NUM >= 18700
            if (!evs.empty()) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);   // finger lifted: no hover
#else
            if (!evs.empty()) io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
#endif
        }

        // ---- snapshot of worker state
        vision::BoardDetect bd; bool bdValid, wb; int imgW, imgH; uint64_t goodMs;
        std::string fen, bm, ev, sfStatus, scanStatus, turn; int evalDepth, scans, analyses; bool learned;
        {
            std::lock_guard<std::mutex> lk(S.m);
            bd = S.bd; bdValid = S.bdValid; wb = S.wb; imgW = S.imgW; imgH = S.imgH; goodMs = S.lastGoodMs;
            fen = S.fen; bm = S.bm; ev = S.ev; sfStatus = S.sfStatus; scanStatus = S.scanStatus;
            evalDepth = S.evalDepth; scans = S.scans; analyses = S.analyses; learned = S.learned; turn = S.turn;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // ---- best-move highlights + arrow: independent of SHOW BEST MOVE (that only controls the text)
        bool haveMove = bdValid && bd.found && bm.size() >= 4 && goodMs && (now - goodMs) < HOLD_MS;
        if (haveMove && imgW > 0 && imgH > 0) {
            float sx = (float)di.width / imgW, sy = (float)di.height / imgH;
            ImDrawList* d = ImGui::GetBackgroundDrawList();
            ImVec2 a = squareCenter(bd, bm.c_str(), wb, sx, sy), z = squareCenter(bd, bm.c_str() + 2, wb, sx, sy);
            float q = bd.size / 8.f * sx, h = q * .5f;
            int al = (int)(255 * ui.arrowOpacity);
            if (ui.showHighlights) {
                d->AddRectFilled(ImVec2(a.x - h, a.y - h), ImVec2(a.x + h, a.y + h), IM_COL32(60, 170, 255, (int)(al * 0.55f)));
                d->AddRect(ImVec2(a.x - h, a.y - h), ImVec2(a.x + h, a.y + h), IM_COL32(60, 170, 255, al), 0, 0, 3.f);
                d->AddRectFilled(ImVec2(z.x - h, z.y - h), ImVec2(z.x + h, z.y + h), IM_COL32(255, 205, 50, (int)(al * 0.55f)));
                d->AddRect(ImVec2(z.x - h, z.y - h), ImVec2(z.x + h, z.y + h), IM_COL32(255, 205, 50, al), 0, 0, 3.f);
            }
            if (ui.showArrow) arrow(d, a, z, ui.arrowThickness, IM_COL32(50, 230, 110, al));
        }

        // ---- panel
        ImGui::SetNextWindowPos(ImVec2(20 * scale, 35 * scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440 * scale, 0), ImGuiCond_Always);
        ImGui::Begin("CHESS ANALYZER");
        ImGui::TextWrapped("%s", sfStatus.c_str());
        ImGui::TextWrapped("BOARD: %s", scanStatus.c_str());
        ImGui::Text("TURN: %s", turn.c_str());
        if (ui.showBestMove) ImGui::Text("BEST MOVE: %s", bm.empty() ? "--" : bm.c_str());
        if (ui.showEval) ImGui::Text("EVAL: %s  (d%d)", ev.empty() ? "--" : ev.c_str(), evalDepth);
        ImGui::TextWrapped("FEN: %s", fen.empty() ? "--" : fen.c_str());
        if (ImGui::Button("SCAN NOW")) g_reqScan = true;
        ImGui::SameLine();
        if (ImGui::Button("REANALYZE")) g_reqReanalyze = true;

        if (ImGui::CollapsingHeader("ANALYZER", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("ON", &ui.analyzer);
            ImGui::Checkbox("AUTO ANALYZE", &ui.autoAnalyze);
            ImGui::Checkbox("SHOW BEST MOVE (text)", &ui.showBestMove);
            ImGui::Checkbox("SHOW ARROW", &ui.showArrow);
            ImGui::Checkbox("SHOW HIGHLIGHTS", &ui.showHighlights);
            ImGui::Checkbox("SHOW EVALUATION", &ui.showEval);
        }
        if (ImGui::CollapsingHeader("BOARD")) {
            ImGui::Text("Orientation");
            ImGui::RadioButton("AUTO", &ui.orientation, -1); ImGui::SameLine();
            ImGui::RadioButton("WHITE", &ui.orientation, 1); ImGui::SameLine();
            ImGui::RadioButton("BLACK", &ui.orientation, 0);
            ImGui::Text("Side to move");
            ImGui::RadioButton("AUTO##stm", &ui.sideToMove, -1); ImGui::SameLine();
            ImGui::RadioButton("WHITE##stm", &ui.sideToMove, 0); ImGui::SameLine();
            ImGui::RadioButton("BLACK##stm", &ui.sideToMove, 1);
            ImGui::Text("Castling");
            ImGui::Checkbox("K", &ck[0]); ImGui::SameLine(); ImGui::Checkbox("Q", &ck[1]); ImGui::SameLine();
            ImGui::Checkbox("k", &ck[2]); ImGui::SameLine(); ImGui::Checkbox("q", &ck[3]);
            const char* epItems = "none\0a\0b\0c\0d\0e\0f\0g\0h\0";
            ImGui::Combo("En passant file", &epIdx, epItems);
            ImGui::Text("Templates: %s  scans=%d analyses=%d", learned ? "LEARNED" : "NOT LEARNED", scans, analyses);
            if (ImGui::Button("LEARN FROM START POSITION")) g_reqLearn = true;
        }
        if (ImGui::CollapsingHeader("STOCKFISH")) {
            ImGui::SliderInt("Depth", &ui.depth, 1, 30);
            ImGui::SliderFloat("Scan interval", &ui.scanInterval, .2f, 5.f, "%.1fs");
            ImGui::SliderFloat("Arrow opacity", &ui.arrowOpacity, .1f, 1.f, "%.2f");
            ImGui::SliderFloat("Arrow thickness", &ui.arrowThickness, 2.f, 24.f, "%.1f");
        }
        if (ImGui::CollapsingHeader("MISC")) {
            ImGui::Text("%s", touch.describe().c_str());
            ImGui::Text("Touch rotation");
            ImGui::RadioButton("AUTO##r", &rotIdx, 0); ImGui::SameLine();
            ImGui::RadioButton("0", &rotIdx, 1); ImGui::SameLine();
            ImGui::RadioButton("90", &rotIdx, 2); ImGui::SameLine();
            ImGui::RadioButton("180", &rotIdx, 3); ImGui::SameLine();
            ImGui::RadioButton("270", &rotIdx, 4);
            syncUi();                                  // make sure `ui` reflects every widget edited so far this frame
            bool dirty = ui.toJson() != savedJson;
            if (ImGui::Button("SAVE SETTINGS")) {
                syncUi();                              // ...and again, immediately before writing
                saveOk = saveSettings(ui);
                if (saveOk) savedJson = ui.toJson();
                savedMsgUntil = now + 3000;
            }
            ImGui::SameLine();
            if (ImGui::Button("EXIT")) { LOG("exit requested from UI"); g_quit = true; }
            if (now < savedMsgUntil) ImGui::Text("%s", saveOk ? "Settings saved" : "SAVE FAILED (see overlay.log)");
            else if (dirty) ImGui::Text("(unsaved changes)");
        }
        ImGui::End();

        // widgets that map to strings
        syncUi();
        { std::lock_guard<std::mutex> lk(S.m); S.cfg = ui; }

        ImGui::Render();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(gd, gs);
        android::ANativeWindowCreator::ProcessMirrorDisplay();
        usleep(16000);
    }

    // ---- clean shutdown: worker (and Stockfish) -> input -> ImGui -> EGL -> window
    LOG("exit: shutting down");
    g_quit = true;
    if (wt.joinable()) wt.join();            // worker stops Stockfish before returning
    touch.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    eglEnd();
    android::ANativeWindowCreator::Destroy(gw);
    gw = nullptr;
    LOG("exit: clean");
    olog::close();
    (void)touchOk;
    return 0;
}
