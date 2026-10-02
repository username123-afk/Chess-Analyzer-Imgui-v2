#pragma once
// Raw touchscreen input for a root, activity-less overlay: reads /dev/input/event* (evdev).
// Independent of ImGui; main.cpp feeds the events into ImGui.
#include <string>
#include <vector>
class TouchInput {
public:
    struct Event { enum Type { Down, Move, Up } type; float x, y; };
    // dispW/dispH: overlay size in pixels. rotation: -1 auto, 0/90/180/270.
    bool init(int dispW, int dispH, int rotation);
    void setRotation(int rotation) { rotCfg_ = rotation; }
    void poll(std::vector<Event>& out);   // non-blocking
    void shutdown();
    int deviceCount() const { return (int)devs_.size(); }
    std::string describe() const;
private:
    struct Dev {
        int fd = -1; std::string path, name;
        int minx = 0, maxx = 0, miny = 0, maxy = 0;
        int slot = 0, rawX = 0, rawY = 0; bool down = false, wasDown = false, moved = false, hasTid = false;
    };
    void map(const Dev& d, float& x, float& y) const;
    std::vector<Dev> devs_;
    int W_ = 0, H_ = 0, rotCfg_ = -1, logged_ = 0;
};
