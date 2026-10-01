#pragma once
#include <string>
#include <sys/types.h>
namespace sf {
struct Result {
    bool ok = false;        // search finished and Stockfish answered
    bool timeout = false;
    std::string bestmove;   // e.g. "e2e4" / "e7e8q"; empty if no legal move
    std::string eval;       // white's point of view: "+0.35", "-1.20", "#3"
    std::string pv;
    std::string err;
    int depth = 0;
};
class Engine {
public:
    ~Engine() { stop(); }
    // Starts the process and verifies uci->uciok and isready->readyok.
    bool start(const std::string& path, std::string& err);
    void stop();            // quit, wait, kill if needed; safe to call repeatedly
    bool running();
    // position fen / isready / readyok / go depth N / parse info + bestmove.
    Result analyze(const std::string& fen, char sideToMove, int depth, int timeoutMs);
    const std::string& name() const { return name_; }
private:
    bool send(const std::string& s);
    int readLine(std::string& line, int ms);   // 1 = line, 0 = timeout, -1 = EOF/error
    bool waitFor(const char* prefix, int ms, std::string* last = nullptr);
    void drain();
    pid_t pid_ = -1;
    int in_ = -1, out_ = -1;
    std::string rbuf_, name_;
};
}
