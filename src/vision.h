// vision.h - screen image, board detection, piece recognition. No platform dependencies.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "chess_core.h"

namespace vision {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> data;  // RGBA8888, row-major
    const uint8_t* at(int x, int y) const { return &data[((size_t)y * w + x) * 4]; }
};

// Parses the output of `screencap` (no -p): [w][h][format](+[colorspace]) + raw pixels.
bool parseRawScreencap(const std::vector<uint8_t>& buf, Image& out, std::string& err);

struct BoardDetect {
    bool found = false;
    float x = 0, y = 0, size = 0;  // board rectangle (square), physical pixels
    int score = 0, maxScore = 0;   // checkerboard agreement
    int ca[3] = {0,0,0}, cb[3] = {0,0,0};  // the two square colours (for fast re-verification)
    int tolUsed = 0;
    std::string why;
};
BoardDetect detectBoard(const Image& im);
// Re-check/track a previously found board on a new frame (tolerant, cheap).
bool verifyBoard(const Image& im, BoardDetect& bd);

constexpr int MG = 40;  // working grid per square
constexpr int TG = 20;  // template grid

struct SqFeat {
    bool empty = true, bad = false;
    float lum = 0;                 // median luminance of piece interior
    float aspect = 1, hfrac = 0;   // bbox aspect and height as fraction of square
    float mask[TG * TG];
};
SqFeat analyzeSquare(const Image& im, float x0, float y0, float s);

struct Sample { float mask[TG * TG]; float aspect, hfrac; };

struct RecogResult {
    bool ok = false;
    char grid[64];        // screen layout: grid[row*8+col], row 0 = top of screen
    int pieces = 0, unknown = 0;
    std::string unknownSquares;
    float worst = 0;      // worst accepted template distance
    std::string why;
};

class Recognizer {
public:
    // Learn piece templates from a board currently showing the standard start position.
    bool learnFromStart(const Image& im, float bx, float by, float size, std::string& err);
    RecogResult recognize(const Image& im, float bx, float by, float size) const;
    bool learned() const { return learned_; }
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    float maxDist = 0.25f;  // template acceptance threshold
private:
    std::vector<Sample> samples_[12];  // index into "PNBRQKpnbrqk"
    float lumThr_ = 128;
    bool learned_ = false;
};

// grid (screen layout) <-> Board
chess::Board gridToBoard(const char grid[64], bool whiteBottom);
// 1 = white at bottom, 0 = black at bottom, -1 = uncertain
int autoOrientation(const char grid[64], std::string& why);

}  // namespace vision
