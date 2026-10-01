#pragma once
#include <string>
struct Settings {
    bool analyzer = true, autoAnalyze = true, showBestMove = true, showArrow = true,
         showHighlights = true, showEval = true;
    int depth = 12;
    float scanInterval = 0.8f, arrowOpacity = 0.75f, arrowThickness = 10.f;
    int orientation = -1;          // -1 auto, 1 white at bottom, 0 black at bottom
    int sideToMove = 0;            // 0 white, 1 black
    std::string castling = "KQkq"; // subset of KQkq or "-"
    std::string enPassant = "-";   // "-" or file letter a-h
    int touchRotation = -1;        // -1 auto, 0/90/180/270
    std::string stockfishPath;     // empty = search default locations
    void clamp();
    std::string toJson() const;
    bool fromJson(const std::string& json);
};
extern const char* kSettingsPath;
bool loadSettings(Settings& s);   // false if missing/unreadable (s keeps defaults)
bool saveSettings(const Settings& s);
