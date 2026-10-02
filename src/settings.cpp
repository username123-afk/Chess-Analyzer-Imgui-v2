#include "settings.h"
#include "log.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

const char* kSettingsPath = "/data/adb/chess_analyzer/run/settings.json";

void Settings::clamp() {
    depth = std::max(1, std::min(30, depth));
    scanInterval = std::max(0.2f, std::min(5.f, scanInterval));
    arrowOpacity = std::max(0.1f, std::min(1.f, arrowOpacity));
    arrowThickness = std::max(2.f, std::min(24.f, arrowThickness));
    if (orientation != 0 && orientation != 1) orientation = -1;
    if (sideToMove != 0 && sideToMove != 1) sideToMove = -1;
    if (touchRotation != 0 && touchRotation != 90 && touchRotation != 180 && touchRotation != 270) touchRotation = -1;
    std::string c;
    for (char ch : std::string("KQkq")) if (castling.find(ch) != std::string::npos) c += ch;
    castling = c.empty() ? "-" : c;
    if (enPassant.empty() || enPassant[0] < 'a' || enPassant[0] > 'h') enPassant = "-"; else enPassant = enPassant.substr(0, 1);
    for (char& ch : stockfishPath) if (ch == '"' || ch == '\\' || ch == '\n') ch = '_';
}

std::string Settings::toJson() const {
    auto b = [](bool v) { return v ? "true" : "false"; };
    char buf[1400];
    snprintf(buf, sizeof buf,
        "{\n"
        "  \"analyzer_enabled\": %s,\n  \"auto_analyze\": %s,\n  \"show_best_move\": %s,\n"
        "  \"show_arrow\": %s,\n  \"show_highlights\": %s,\n  \"show_evaluation\": %s,\n"
        "  \"stockfish_depth\": %d,\n  \"scan_interval\": %.2f,\n  \"arrow_opacity\": %.2f,\n"
        "  \"arrow_thickness\": %.1f,\n  \"board_orientation\": %d,\n  \"side_to_move_mode\": %d,\n"
        "  \"castling\": \"%s\",\n  \"en_passant\": \"%s\",\n  \"touch_rotation\": %d,\n"
        "  \"stockfish_path\": \"%s\"\n}\n",
        b(analyzer), b(autoAnalyze), b(showBestMove), b(showArrow), b(showHighlights), b(showEval),
        depth, scanInterval, arrowOpacity, arrowThickness, orientation, sideToMove,
        castling.c_str(), enPassant.c_str(), touchRotation, stockfishPath.c_str());
    return buf;
}

static bool findVal(const std::string& j, const char* key, std::string& out) {
    size_t k = j.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return false;
    size_t c = j.find(':', k);
    if (c == std::string::npos) return false;
    c++;
    while (c < j.size() && (j[c] == ' ' || j[c] == '\t' || j[c] == '\n' || j[c] == '\r')) c++;
    if (c >= j.size()) return false;
    if (j[c] == '"') {
        size_t e = j.find('"', c + 1);
        if (e == std::string::npos) return false;
        out = j.substr(c + 1, e - c - 1);
    } else {
        size_t e = j.find_first_of(",}\n\r", c);
        out = j.substr(c, e == std::string::npos ? std::string::npos : e - c);
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return true;
}

bool Settings::fromJson(const std::string& j) {
    std::string v; int found = 0;
    auto gb = [&](const char* k, bool& d) { if (findVal(j, k, v)) { d = (v == "true" || v == "1"); found++; } };
    auto gi = [&](const char* k, int& d) { if (findVal(j, k, v)) { d = atoi(v.c_str()); found++; } };
    auto gf = [&](const char* k, float& d) { if (findVal(j, k, v)) { d = (float)atof(v.c_str()); found++; } };
    auto gs = [&](const char* k, std::string& d) { if (findVal(j, k, v)) { d = v; found++; } };
    gb("analyzer_enabled", analyzer); gb("auto_analyze", autoAnalyze); gb("show_best_move", showBestMove);
    gb("show_arrow", showArrow); gb("show_highlights", showHighlights); gb("show_evaluation", showEval);
    gi("stockfish_depth", depth); gf("scan_interval", scanInterval); gf("arrow_opacity", arrowOpacity);
    gf("arrow_thickness", arrowThickness); gi("board_orientation", orientation); gi("side_to_move_mode", sideToMove);
    gs("castling", castling); gs("en_passant", enPassant); gi("touch_rotation", touchRotation);
    gs("stockfish_path", stockfishPath);
    clamp();
    return found > 0;
}

bool loadSettings(Settings& s) {
    std::ifstream f(kSettingsPath);
    if (!f) { LOG("settings: %s not found, using defaults", kSettingsPath); return false; }
    std::stringstream ss; ss << f.rdbuf();
    bool ok = s.fromJson(ss.str());
    LOG("settings: loaded from %s (%s)", kSettingsPath, ok ? "ok" : "no recognised keys, defaults kept");
    return ok;
}

bool saveSettings(const Settings& in) {
    Settings s = in; s.clamp();
    std::string tmp = std::string(kSettingsPath) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { LOG("settings: cannot write %s", tmp.c_str()); return false; }
        f << s.toJson();
        f.flush();
        if (!f) { LOG("settings: write failed"); return false; }
    }
    if (rename(tmp.c_str(), kSettingsPath) != 0) { LOG("settings: rename failed"); return false; }
    LOG("settings: saved to %s (depth=%d interval=%.2f orient=%d side=%d castling=%s ep=%s)", kSettingsPath,
        s.depth, s.scanInterval, s.orientation, s.sideToMove, s.castling.c_str(), s.enPassant.c_str());
    return true;
}
