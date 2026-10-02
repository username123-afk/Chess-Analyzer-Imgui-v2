#include "vision.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace vision {

static inline int iabs(int a) { return a < 0 ? -a : a; }
static const char* kPieces = "PNBRQKpnbrqk";
static int pieceIndex(char c) {
    const char* p = strchr(kPieces, c);
    return p ? (int)(p - kPieces) : -1;
}

// ---------------------------------------------------------------- screencap parsing
bool parseRawScreencap(const std::vector<uint8_t>& buf, Image& out, std::string& err) {
    if (buf.size() < 16) { err = "empty screencap output"; return false; }
    uint32_t w, h, fmt;
    memcpy(&w, &buf[0], 4); memcpy(&h, &buf[4], 4); memcpy(&fmt, &buf[8], 4);
    if (w < 64 || h < 64 || w > 16384 || h > 16384) { err = "bad screencap header"; return false; }
    size_t pix = (size_t)w * h * 4, hdr;
    if (buf.size() == 12 + pix) hdr = 12;
    else if (buf.size() == 16 + pix) hdr = 16;
    else { err = "screencap size mismatch"; return false; }
    if (fmt != 1 && fmt != 2 && fmt != 5) { err = "unsupported screencap pixel format " + std::to_string(fmt); return false; }
    out.w = (int)w; out.h = (int)h;
    out.data.assign(buf.begin() + hdr, buf.begin() + hdr + pix);
    for (size_t i = 0; i < pix; i += 4) {
        if (fmt == 5) std::swap(out.data[i], out.data[i + 2]);  // BGRA -> RGBA
        out.data[i + 3] = 255;
    }
    return true;
}

// ---------------------------------------------------------------- board detection
struct Col { int r, g, b; };
static inline bool nearCol(const uint8_t* p, const Col& c, int tol) {
    return iabs(p[0] - c.r) + iabs(p[1] - c.g) + iabs(p[2] - c.b) < tol;
}
static const int kTol = 24;
static const int kSamples = 6;
static const float kOff[kSamples][2] = {{.5f,.07f},{.5f,.93f},{.07f,.5f},{.93f,.5f},{.15f,.85f},{.85f,.15f}};

// Checkerboard agreement for a candidate board rectangle (max 64*6).
static int gridScore(const Image& im, const Col& A, const Col& B, float bx, float by, float S, int tol) {
    if (bx < 0 || by < 0 || bx + S > im.w || by + S > im.h) return 0;
    float s = S / 8.f;
    int sx = 0, sy = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            bool even = ((r + c) & 1) == 0;
            for (int k = 0; k < kSamples; k++) {
                int px = std::min(im.w - 1, (int)(bx + (c + kOff[k][0]) * s));
                int py = std::min(im.h - 1, (int)(by + (r + kOff[k][1]) * s));
                const uint8_t* p = im.at(px, py);
                bool a = nearCol(p, A, tol), b = nearCol(p, B, tol);
                if (even ? a : b) sx++;
                if (even ? b : a) sy++;
            }
        }
    return std::max(sx, sy);
}

static bool longestRun(const std::vector<int>& v, int thr, int gap, int& s, int& e) {
    int bestLen = -1, start = -1, last = -1;
    s = e = 0;
    for (int i = 0; i <= (int)v.size(); i++) {
        bool on = i < (int)v.size() && v[i] >= thr;
        if (on) { if (start < 0) start = i; last = i; }
        else if (start >= 0 && (i >= (int)v.size() || i - last > gap)) {
            if (last - start > bestLen) { bestLen = last - start; s = start; e = last; }
            start = -1;
        }
    }
    return bestLen >= 0;
}

static BoardDetect detectBoardTol(const Image& im, int tol, float thr) {
    BoardDetect bd;
    bd.tolUsed = tol;
    if (im.w < 200 || im.h < 200) { bd.why = "image too small"; return bd; }
    const int stride = 2;

    // 1) dominant colours (4-bit quantised), their mean colours
    std::vector<int> cnt(4096, 0);
    std::vector<long> sr(4096, 0), sg(4096, 0), sb(4096, 0);
    long total = 0;
    for (int y = 0; y < im.h; y += stride)
        for (int x = 0; x < im.w; x += stride) {
            const uint8_t* p = im.at(x, y);
            int k = ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4);
            cnt[k]++; sr[k] += p[0]; sg[k] += p[1]; sb[k] += p[2]; total++;
        }
    std::vector<int> idx(4096);
    std::iota(idx.begin(), idx.end(), 0);
    const int K = 8;
    std::partial_sort(idx.begin(), idx.begin() + K, idx.end(), [&](int a, int b) { return cnt[a] > cnt[b]; });
    std::vector<Col> cols;
    for (int i = 0; i < K; i++) {
        int k = idx[i];
        if (cnt[k] < total * 0.004) break;
        cols.push_back({(int)(sr[k] / cnt[k]), (int)(sg[k] / cnt[k]), (int)(sb[k] / cnt[k])});
    }

    // 2) every colour pair is a candidate (light square, dark square)
    int bestScore = 0;
    int H = (im.h + stride - 1) / stride, W = (im.w + stride - 1) / stride;
    for (size_t i = 0; i < cols.size(); i++)
        for (size_t j = i + 1; j < cols.size(); j++) {
            const Col &A = cols[i], &B = cols[j];
            int cd = iabs(A.r - B.r) + iabs(A.g - B.g) + iabs(A.b - B.b);
            if (cd < 24 || cd > 450) continue;
            std::vector<int> rowc(H, 0), colc(W, 0);
            for (int y = 0; y < im.h; y += stride)
                for (int x = 0; x < im.w; x += stride) {
                    const uint8_t* p = im.at(x, y);
                    if (nearCol(p, A, tol) || nearCol(p, B, tol)) rowc[y / stride]++;
                }
            int rmax = *std::max_element(rowc.begin(), rowc.end());
            int ys, ye;
            if (rmax < 40 || !longestRun(rowc, (int)(rmax * 0.2), std::max(2, H / 50), ys, ye)) continue;
            for (int y = ys * stride; y <= std::min(im.h - 1, ye * stride); y += stride)
                for (int x = 0; x < im.w; x += stride) {
                    const uint8_t* p = im.at(x, y);
                    if (nearCol(p, A, tol) || nearCol(p, B, tol)) colc[x / stride]++;
                }
            int cmax = *std::max_element(colc.begin(), colc.end());
            int xs, xe;
            if (cmax < 20 || !longestRun(colc, (int)(cmax * 0.2), std::max(2, W / 50), xs, xe)) continue;
            float x0 = xs * stride, x1 = xe * stride + stride, y0 = ys * stride, y1 = ye * stride + stride;
            float w = x1 - x0, h = y1 - y0;
            if (std::min(w, h) < 0.25f * std::min(im.w, im.h)) continue;
            if (std::max(w, h) / std::min(w, h) > 1.6f) {}  // tolerated: refinement searches sizes

            // The colour-run bounding box is usually the exact board rectangle. The grid score is flat
            // near the optimum (a few px off scores about the same), so a blind search can settle ~10px
            // off, which makes pieces bleed into neighbouring squares. Try the exact box first and make
            // every other candidate beat it by a clear margin.
            int margin = 0;
            if (std::max(w, h) / std::min(w, h) < 1.05f) {
                float S0 = (w + h) * 0.5f;
                int sc = gridScore(im, A, B, x0, y0, S0, tol);
                if (sc > bestScore) {
                    bestScore = sc; bd.x = x0; bd.y = y0; bd.size = S0;
                    bd.ca[0]=A.r; bd.ca[1]=A.g; bd.ca[2]=A.b; bd.cb[0]=B.r; bd.cb[1]=B.g; bd.cb[2]=B.b;
                    margin = (int)(0.02f * 64 * kSamples);
                }
            }

            float sizes[3] = {w, h, (w + h) * 0.5f};
            for (float S0 : sizes)
                for (int oxi = 0; oxi < 2; oxi++)
                    for (int oyi = 0; oyi < 2; oyi++) {
                        float ox = oxi ? x1 - S0 : x0, oy = oyi ? y1 - S0 : y0;
                        for (int dsi = -2; dsi <= 2; dsi++) {
                            float S = S0 * (1.f + 0.015f * dsi);
                            float d = S / 8.f * 0.35f, st = std::max(1.f, S / 120.f);
                            for (float dy = -d; dy <= d; dy += st)
                                for (float dx = -d; dx <= d; dx += st) {
                                    int sc = gridScore(im, A, B, ox + dx, oy + dy, S, tol);
                                    if (sc > bestScore + margin) {
                                        bestScore = sc; bd.x = ox + dx; bd.y = oy + dy; bd.size = S;
                                        bd.ca[0]=A.r; bd.ca[1]=A.g; bd.ca[2]=A.b; bd.cb[0]=B.r; bd.cb[1]=B.g; bd.cb[2]=B.b;
                                    }
                                }
                        }
                    }
            // fine pass around the running best
            if (bestScore > 0) {
                float bx = bd.x, by = bd.y, bs = bd.size;
                for (float ds = -bs * 0.01f; ds <= bs * 0.01f; ds += std::max(1.f, bs * 0.005f))
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2; dx <= 2; dx++) {
                            int sc = gridScore(im, A, B, bx + dx, by + dy, bs + ds, tol);
                            if (sc > bestScore + margin) { bestScore = sc; bd.x = bx + dx; bd.y = by + dy; bd.size = bs + ds;
                                bd.ca[0]=A.r; bd.ca[1]=A.g; bd.ca[2]=A.b; bd.cb[0]=B.r; bd.cb[1]=B.g; bd.cb[2]=B.b; }
                        }
            }
        }
    bd.score = bestScore;
    bd.maxScore = 64 * kSamples;
    if (bestScore >= (int)(thr * bd.maxScore) && bd.size >= 0.25f * std::min(im.w, im.h)) bd.found = true;
    else bd.why = "no 8x8 checkerboard found (best agreement " + std::to_string(bestScore) + "/" +
                  std::to_string(bd.maxScore) + ", needed " + std::to_string((int)(thr * bd.maxScore)) +
                  ", colour tolerance " + std::to_string(tol) + ")";
    return bd;
}

// Tolerant detection: strict pass first, then progressively looser colour tolerance
// (handles themes with gradients, brightness changes, dimmed boards, anti-aliased edges).
BoardDetect detectBoard(const Image& im) {
    static const int tols[3] = {24, 40, 58};
    static const float thrs[3] = {0.62f, 0.56f, 0.52f};
    BoardDetect best;
    for (int i = 0; i < 3; i++) {
        BoardDetect bd = detectBoardTol(im, tols[i], thrs[i]);
        if (bd.found) return bd;
        if (bd.score >= best.score) best = bd;
    }
    return best;
}

// Cheap re-check of a previously found board: small position/size jitter, loose tolerance.
bool verifyBoard(const Image& im, BoardDetect& bd) {
    if (!bd.found || im.w < 200 || im.h < 200) return false;
    Col A{bd.ca[0], bd.ca[1], bd.ca[2]}, B{bd.cb[0], bd.cb[1], bd.cb[2]};
    int best = gridScore(im, A, B, bd.x, bd.y, bd.size, 48); float bx = bd.x, by = bd.y, bs = bd.size;
    const int need = best + 3;   // only move for a clear gain, otherwise the rectangle would creep
    int cur = best;
    for (int ds = -1; ds <= 1; ds++)
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                float S = bd.size * (1.f + 0.01f * ds);
                int sc = gridScore(im, A, B, bd.x + dx, bd.y + dy, S, 48);
                if (sc >= need && sc > best) { best = sc; bx = bd.x + dx; by = bd.y + dy; bs = S; }
            }
    (void)cur;
    if (best < (int)(0.55f * bd.maxScore)) return false;
    bd.x = bx; bd.y = by; bd.size = bs; bd.score = best;
    return true;
}

// ---------------------------------------------------------------- per-square analysis
SqFeat analyzeSquare(const Image& im, float x0, float y0, float s) {
    SqFeat f;
    memset(f.mask, 0, sizeof(f.mask));
    const int N = MG;
    float cs = s / N;
    static thread_local float col[N * N][3];
    auto clampx = [&](float v) { return std::max(0, std::min(im.w - 1, (int)v)); };
    auto clampy = [&](float v) { return std::max(0, std::min(im.h - 1, (int)v)); };
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            float r = 0, g = 0, b = 0;
            for (int q = 0; q < 4; q++) {
                float fx = x0 + (i + 0.25f + 0.5f * (q & 1)) * cs, fy = y0 + (j + 0.25f + 0.5f * (q >> 1)) * cs;
                const uint8_t* p = im.at(clampx(fx), clampy(fy));
                r += p[0]; g += p[1]; b += p[2];
            }
            col[j * N + i][0] = r / 4; col[j * N + i][1] = g / 4; col[j * N + i][2] = b / 4;
        }
    // background colour = most common quantised colour on an outer ring
    static thread_local int hist[4096];
    memset(hist, 0, sizeof(hist));
    auto key = [&](int c) {
        return ((int)col[c][0] >> 4) << 8 | ((int)col[c][1] >> 4) << 4 | ((int)col[c][2] >> 4);
    };
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            int m = std::min(std::min(i, j), std::min(N - 1 - i, N - 1 - j));
            if (m == 2 || m == 3) hist[key(j * N + i)]++;
        }
    int bk = (int)(std::max_element(hist, hist + 4096) - hist);
    float bg[3] = {0, 0, 0}; int bn = 0;
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            int m = std::min(std::min(i, j), std::min(N - 1 - i, N - 1 - j));
            if ((m == 2 || m == 3) && key(j * N + i) == bk) {
                for (int c = 0; c < 3; c++) bg[c] += col[j * N + i][c];
                bn++;
            }
        }
    for (int c = 0; c < 3; c++) bg[c] /= std::max(1, bn);

    static thread_local uint8_t fg[N * N], outside[N * N], reg[N * N];
    static thread_local int stack[N * N], comp[N * N], best[N * N];
    for (int c = 0; c < N * N; c++) {
        float d = std::fabs(col[c][0] - bg[0]) + std::fabs(col[c][1] - bg[1]) + std::fabs(col[c][2] - bg[2]);
        fg[c] = d > 120.f;
    }
    // flood from border through non-fg cells -> "outside"; enclosed holes become part of the piece
    memset(outside, 0, sizeof(outside));
    int sp = 0;
    for (int i = 0; i < N; i++) {
        int cells[4] = {i, (N - 1) * N + i, i * N, i * N + N - 1};
        for (int c : cells)
            if (!fg[c] && !outside[c]) { outside[c] = 1; stack[sp++] = c; }
    }
    while (sp) {
        int c = stack[--sp], x = c % N, y = c / N;
        int nb[4] = {x > 0 ? c - 1 : -1, x < N - 1 ? c + 1 : -1, y > 0 ? c - N : -1, y < N - 1 ? c + N : -1};
        for (int n : nb)
            if (n >= 0 && !fg[n] && !outside[n]) { outside[n] = 1; stack[sp++] = n; }
    }
    for (int c = 0; c < N * N; c++) reg[c] = fg[c] || !outside[c];

    // largest 8-connected component
    std::vector<uint8_t> seen(N * N, 0);
    int bestN = 0;
    for (int c0 = 0; c0 < N * N; c0++) {
        if (!reg[c0] || seen[c0]) continue;
        int n = 0; sp = 0;
        stack[sp++] = c0; seen[c0] = 1;
        while (sp) {
            int c = stack[--sp]; comp[n++] = c;
            int x = c % N, y = c / N;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue;
                    int nc = ny * N + nx;
                    if (reg[nc] && !seen[nc]) { seen[nc] = 1; stack[sp++] = nc; }
                }
        }
        if (n > bestN) { bestN = n; memcpy(best, comp, n * sizeof(int)); }
    }
    if (bestN < 48) { f.empty = true; return f; }
    f.empty = false;
    if (bestN > 1100) { f.bad = true; return f; }

    std::vector<uint8_t> in(N * N, 0);
    int minx = N, maxx = 0, miny = N, maxy = 0;
    for (int k = 0; k < bestN; k++) {
        int c = best[k]; in[c] = 1;
        minx = std::min(minx, c % N); maxx = std::max(maxx, c % N);
        miny = std::min(miny, c / N); maxy = std::max(maxy, c / N);
    }
    // median luminance of interior cells (all 4 neighbours inside the piece)
    std::vector<float> lums, all;
    for (int k = 0; k < bestN; k++) {
        int c = best[k], x = c % N, y = c / N;
        float L = 0.299f * col[c][0] + 0.587f * col[c][1] + 0.114f * col[c][2];
        all.push_back(L);
        if (x > 0 && y > 0 && x < N - 1 && y < N - 1 && in[c - 1] && in[c + 1] && in[c - N] && in[c + N]) lums.push_back(L);
    }
    std::vector<float>& L = lums.size() >= 10 ? lums : all;
    std::nth_element(L.begin(), L.begin() + L.size() / 2, L.end());
    f.lum = L[L.size() / 2];

    float bw = (float)(maxx - minx + 1), bh = (float)(maxy - miny + 1);
    f.aspect = bw / bh;
    f.hfrac = bh / N;
    for (int v = 0; v < TG; v++)
        for (int u = 0; u < TG; u++) {
            int hit = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    int x = minx + (int)(((u + (sx + 0.5f) / 3.f) / TG) * bw);
                    int y = miny + (int)(((v + (sy + 0.5f) / 3.f) / TG) * bh);
                    x = std::min(N - 1, x); y = std::min(N - 1, y);
                    hit += in[y * N + x];
                }
            f.mask[v * TG + u] = hit / 9.f;
        }
    return f;
}

static float sampleDist(const SqFeat& f, const Sample& s) {
    float d = 0;
    for (int i = 0; i < TG * TG; i++) d += std::fabs(f.mask[i] - s.mask[i]);
    d /= (TG * TG);
    return d + 0.5f * std::fabs(f.aspect - s.aspect) + 1.0f * std::fabs(f.hfrac - s.hfrac);
}

// ---------------------------------------------------------------- recognizer
bool Recognizer::learnFromStart(const Image& im, float bx, float by, float size, std::string& err) {
    float s = size / 8.f;
    SqFeat feats[64];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) feats[r * 8 + c] = analyzeSquare(im, bx + c * s, by + r * s, s);
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            bool expectPiece = (r <= 1 || r >= 6);
            if (feats[r * 8 + c].bad) { err = "square unreadable (highlight/overlay?)"; return false; }
            if (feats[r * 8 + c].empty == expectPiece) {
                err = "board is not showing the standard start position";
                return false;
            }
        }
    auto rowLum = [&](int r0, int r1) {
        float t = 0; int n = 0;
        for (int r = r0; r <= r1; r++) for (int c = 0; c < 8; c++) { t += feats[r * 8 + c].lum; n++; }
        return t / n;
    };
    float top = rowLum(0, 1), bot = rowLum(6, 7);
    if (std::fabs(top - bot) < 40.f) { err = "cannot tell white pieces from black pieces"; return false; }
    bool whiteBottom = bot > top;
    lumThr_ = (top + bot) * 0.5f;

    for (auto& v : samples_) v.clear();
    chess::Board sb = chess::startBoard();
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            if (feats[r * 8 + c].empty) continue;
            int file = whiteBottom ? c : 7 - c, rank = whiteBottom ? 7 - r : r;
            int pi = pieceIndex(sb.at(file, rank));
            Sample smp;
            memcpy(smp.mask, feats[r * 8 + c].mask, sizeof(smp.mask));
            smp.aspect = feats[r * 8 + c].aspect; smp.hfrac = feats[r * 8 + c].hfrac;
            samples_[pi].push_back(smp);
        }
    learned_ = true;
    // self-test: the learned templates must reproduce the start position
    RecogResult rr = recognize(im, bx, by, size);
    if (!rr.ok) { learned_ = false; err = "self-test failed: " + rr.why; return false; }
    chess::Board got = gridToBoard(rr.grid, whiteBottom);
    if (!(got == sb)) { learned_ = false; err = "self-test failed: start position mismatch"; return false; }
    return true;
}

RecogResult Recognizer::recognize(const Image& im, float bx, float by, float size) const {
    RecogResult rr;
    memset(rr.grid, '.', 64);
    if (!learned_) { rr.why = "no piece templates learned yet"; return rr; }
    float s = size / 8.f;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            SqFeat f = analyzeSquare(im, bx + c * s, by + r * s, s);
            if (f.empty) continue;
            if (f.bad) { rr.unknown++; rr.unknownSquares += " r" + std::to_string(r+1) + "c" + std::to_string(c+1) + "(overlay/highlight)"; continue; }
            bool white = f.lum > lumThr_;
            int lo = white ? 0 : 6;
            float best = 1e9f; int bi = -1;
            for (int k = lo; k < lo + 6; k++)
                for (const Sample& smp : samples_[k]) {
                    float d = sampleDist(f, smp);
                    if (d < best) { best = d; bi = k; }
                }
            if (bi < 0 || best > maxDist) { rr.unknown++; char t[48]; snprintf(t, sizeof t, " r%dc%d(d=%.2f)", r+1, c+1, best); rr.unknownSquares += t; continue; }
            rr.grid[r * 8 + c] = kPieces[bi];
            rr.pieces++;
            rr.worst = std::max(rr.worst, best);
        }
    if (rr.unknown) { rr.why = std::to_string(rr.unknown) + " square(s) not recognised (row/col from top-left):" + rr.unknownSquares; return rr; }
    rr.ok = true;
    return rr;
}

bool Recognizer::save(const std::string& path) const {
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return false;
    uint32_t magic = 0x43415431;  // "CAT1"
    fwrite(&magic, 4, 1, fp);
    fwrite(&lumThr_, 4, 1, fp);
    for (auto& v : samples_) {
        uint32_t n = (uint32_t)v.size();
        fwrite(&n, 4, 1, fp);
        for (auto& s : v) fwrite(&s, sizeof(Sample), 1, fp);
    }
    fclose(fp);
    return true;
}

bool Recognizer::load(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    uint32_t magic = 0;
    bool ok = fread(&magic, 4, 1, fp) == 1 && magic == 0x43415431 && fread(&lumThr_, 4, 1, fp) == 1;
    for (auto& v : samples_) {
        v.clear();
        uint32_t n = 0;
        if (ok) ok = fread(&n, 4, 1, fp) == 1 && n <= 64;
        for (uint32_t i = 0; ok && i < n; i++) {
            Sample s;
            ok = fread(&s, sizeof(Sample), 1, fp) == 1;
            if (ok) v.push_back(s);
        }
    }
    fclose(fp);
    learned_ = ok;
    return ok;
}

// ---------------------------------------------------------------- orientation / conversion
chess::Board gridToBoard(const char grid[64], bool whiteBottom) {
    chess::Board b;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int file = whiteBottom ? c : 7 - c, rank = whiteBottom ? 7 - r : r;
            b.at(file, rank) = grid[r * 8 + c];
        }
    return b;
}

int autoOrientation(const char grid[64], std::string& why) {
    std::string e1, e2;
    bool okW = chess::validate(gridToBoard(grid, true), true, false, e1);
    bool okB = chess::validate(gridToBoard(grid, false), true, false, e2);
    if (!okW && !okB) { why = "position invalid in both orientations"; return -1; }
    if (okW && !okB) return 1;
    if (okB && !okW) return 0;
    // both structurally valid: white pieces should sit lower on screen than black pieces
    float ws = 0, bs = 0; int wn = 0, bn = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            char p = grid[r * 8 + c];
            if (p == '.') continue;
            if (chess::isWhite(p)) { ws += r; wn++; } else { bs += r; bn++; }
        }
    if (!wn || !bn) { why = "orientation uncertain"; return -1; }
    float diff = ws / wn - bs / bn;
    if (diff > 0.5f) return 1;
    if (diff < -0.5f) return 0;
    why = "orientation uncertain";
    return -1;
}

}  // namespace vision
