#include "touch_input.h"
#include "log.h"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif
#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#endif
#ifndef INPUT_PROP_DIRECT
#define INPUT_PROP_DIRECT 0x01
#endif

static bool testBit(const unsigned char* a, int bit) { return a[bit / 8] & (1 << (bit % 8)); }

bool TouchInput::init(int W, int H, int rotation) {
    W_ = W; H_ = H; rotCfg_ = rotation;
    DIR* d = opendir("/dev/input");
    if (!d) { LOG("input: cannot open /dev/input (errno=%d)", errno); return false; }
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char abs[ABS_MAX / 8 + 1] = {0}, prop[INPUT_PROP_MAX / 8 + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        ioctl(fd, EVIOCGPROP(sizeof prop), prop);
        bool mt = testBit(abs, ABS_MT_POSITION_X) && testBit(abs, ABS_MT_POSITION_Y);
        bool direct = testBit(prop, INPUT_PROP_DIRECT);
        char name[128] = "?"; ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (!mt || !direct) { close(fd); continue; }       // keep only touchscreens
        input_absinfo ax{}, ay{};
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
        if (ax.maximum <= ax.minimum || ay.maximum <= ay.minimum) { close(fd); continue; }
        Dev dv; dv.fd = fd; dv.path = path; dv.name = name;
        dv.minx = ax.minimum; dv.maxx = ax.maximum; dv.miny = ay.minimum; dv.maxy = ay.maximum;
        devs_.push_back(dv);
        LOG("input: touchscreen %s \"%s\" x=[%d..%d] y=[%d..%d]", path.c_str(), name, dv.minx, dv.maxx, dv.miny, dv.maxy);
    }
    closedir(d);
    if (devs_.empty()) LOG("input: NO touchscreen device found in /dev/input (need root; run `getevent -lp`)");
    return !devs_.empty();
}

void TouchInput::map(const Dev& d, float& x, float& y) const {
    float nx = (float)(d.rawX - d.minx) / (float)(d.maxx - d.minx);
    float ny = (float)(d.rawY - d.miny) / (float)(d.maxy - d.miny);
    nx = nx < 0 ? 0 : (nx > 1 ? 1 : nx); ny = ny < 0 ? 0 : (ny > 1 ? 1 : ny);
    int rot = rotCfg_;
    if (rot < 0) {  // auto: panel is natively portrait; landscape display => 90
        bool panelPortrait = (d.maxy - d.miny) >= (d.maxx - d.minx);
        bool dispPortrait = H_ >= W_;
        rot = (panelPortrait == dispPortrait) ? 0 : 90;
    }
    switch (rot) {
        case 90:  x = ny * W_;        y = (1.f - nx) * H_; break;
        case 180: x = (1.f - nx) * W_; y = (1.f - ny) * H_; break;
        case 270: x = (1.f - ny) * W_; y = nx * H_;        break;
        default:  x = nx * W_;        y = ny * H_;         break;
    }
}

void TouchInput::poll(std::vector<Event>& out) {
    for (Dev& d : devs_) {
        input_event ev[64];
        for (;;) {
            ssize_t n = read(d.fd, ev, sizeof ev);
            if (n <= 0) break;
            for (size_t i = 0; i < (size_t)n / sizeof(input_event); i++) {
                const input_event& e = ev[i];
                if (e.type == EV_ABS) {
                    if (e.code == ABS_MT_SLOT) d.slot = e.value;
                    else if (d.slot == 0) {   // follow the first finger only
                        if (e.code == ABS_MT_POSITION_X) { d.rawX = e.value; d.moved = true; }
                        else if (e.code == ABS_MT_POSITION_Y) { d.rawY = e.value; d.moved = true; }
                        else if (e.code == ABS_MT_TRACKING_ID) d.down = e.value >= 0;
                    }
                } else if (e.type == EV_KEY && e.code == BTN_TOUCH) {
                    d.down = e.value != 0;
                } else if (e.type == EV_SYN && e.code == SYN_REPORT) {
                    float x, y; map(d, x, y);
                    if (d.down && !d.wasDown) {
                        out.push_back({Event::Down, x, y});
                        if (logged_ < 8) { LOG("input: touch down raw=(%d,%d) -> screen=(%.0f,%.0f)", d.rawX, d.rawY, x, y); logged_++; }
                    } else if (d.down && d.moved) out.push_back({Event::Move, x, y});
                    else if (!d.down && d.wasDown) out.push_back({Event::Up, x, y});
                    d.wasDown = d.down; d.moved = false;
                }
            }
        }
    }
}

void TouchInput::shutdown() { for (Dev& d : devs_) if (d.fd >= 0) close(d.fd); devs_.clear(); }

std::string TouchInput::describe() const {
    if (devs_.empty()) return "INPUT: NO TOUCH DEVICE";
    return "INPUT: evdev " + devs_[0].name + (devs_.size() > 1 ? " +" + std::to_string(devs_.size() - 1) : "");
}
