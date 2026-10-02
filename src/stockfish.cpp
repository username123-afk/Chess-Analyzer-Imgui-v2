#include "stockfish.h"
#include "log.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <poll.h>
#include <sstream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sf {
using Clock = std::chrono::steady_clock;
static int msLeft(Clock::time_point deadline) {
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
}
static bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

bool Engine::start(const std::string& path, std::string& err) {
    stop();
    if (access(path.c_str(), F_OK) != 0) { err = "binary not found: " + path; return false; }
    if (access(path.c_str(), X_OK) != 0) chmod(path.c_str(), 0755);
    if (access(path.c_str(), X_OK) != 0) { err = "binary not executable: " + path; return false; }
    int a[2], b[2];
    if (pipe(a) || pipe(b)) { err = "pipe() failed"; return false; }
    pid_t pid = fork();
    if (pid < 0) { close(a[0]); close(a[1]); close(b[0]); close(b[1]); err = "fork() failed"; return false; }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);   // never leave an orphan engine behind
        dup2(a[0], 0); dup2(b[1], 1); dup2(b[1], 2);
        close(a[0]); close(a[1]); close(b[0]); close(b[1]);
        execl(path.c_str(), path.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(a[0]); close(b[1]);
    pid_ = pid; in_ = a[1]; out_ = b[0]; rbuf_.clear(); name_.clear();
    LOG("stockfish: process started pid=%d path=%s", (int)pid, path.c_str());
    send("uci");
    std::string last;
    if (!waitFor("uciok", 5000, &last)) { err = "no 'uciok' from engine"; stop(); return false; }
    LOG("stockfish: uciok (%s)", name_.empty() ? "unnamed engine" : name_.c_str());
    send("isready");
    if (!waitFor("readyok", 5000)) { err = "no 'readyok' from engine"; stop(); return false; }
    LOG("stockfish: readyok");
    return true;
}

void Engine::stop() {
    if (pid_ > 0) {
        send("quit");
        bool gone = false;
        for (int i = 0; i < 50 && !gone; i++) {
            int st; pid_t r = waitpid(pid_, &st, WNOHANG);
            if (r == pid_ || (r < 0 && errno == ECHILD)) gone = true; else usleep(10000);
        }
        if (!gone) { kill(pid_, SIGKILL); waitpid(pid_, nullptr, 0); }
        LOG("stockfish: stopped%s", gone ? "" : " (killed)");
    }
    if (in_ >= 0) close(in_);
    if (out_ >= 0) close(out_);
    pid_ = -1; in_ = out_ = -1; rbuf_.clear();
}

bool Engine::running() {
    if (pid_ <= 0) return false;
    int st; pid_t r = waitpid(pid_, &st, WNOHANG);
    if (r == 0) return true;
    LOG("stockfish: process %d exited unexpectedly (status=0x%x)", (int)pid_, r == pid_ ? st : 0);
    if (in_ >= 0) close(in_);
    if (out_ >= 0) close(out_);
    pid_ = -1; in_ = out_ = -1; rbuf_.clear();
    return false;
}

bool Engine::send(const std::string& s) {
    if (in_ < 0) return false;
    std::string x = s + "\n";
    const char* p = x.data(); size_t left = x.size();
    while (left) {
        ssize_t n = write(in_, p, left);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        p += n; left -= (size_t)n;
    }
    return true;
}

int Engine::readLine(std::string& line, int ms) {
    auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        size_t pos = rbuf_.find('\n');
        if (pos != std::string::npos) {
            line = rbuf_.substr(0, pos);
            rbuf_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return 1;
        }
        int left = msLeft(deadline);
        if (left < 0) left = 0;
        pollfd p{out_, POLLIN, 0};
        int r = poll(&p, 1, left);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return 0;
        char buf[4096];
        ssize_t n = read(out_, buf, sizeof buf);
        if (n > 0) rbuf_.append(buf, (size_t)n);
        else if (n == 0) return -1;
        else if (errno != EINTR && errno != EAGAIN) return -1;
    }
}

bool Engine::waitFor(const char* prefix, int ms, std::string* last) {
    auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    std::string line;
    for (;;) {
        int left = msLeft(deadline);
        if (left <= 0) return false;
        int r = readLine(line, left);
        if (r <= 0) return false;
        if (startsWith(line, "id name ")) name_ = line.substr(8);
        if (startsWith(line, prefix)) { if (last) *last = line; return true; }
    }
}

void Engine::drain() {
    std::string l;
    while (readLine(l, 0) > 0) {}
    rbuf_.clear();
}

static bool validMove(const std::string& m) {
    if (m.size() != 4 && m.size() != 5) return false;
    return m[0] >= 'a' && m[0] <= 'h' && m[2] >= 'a' && m[2] <= 'h' &&
           m[1] >= '1' && m[1] <= '8' && m[3] >= '1' && m[3] <= '8';
}

Result Engine::analyze(const std::string& fen, char stm, int depth, int timeoutMs) {
    Result r;
    if (!running()) { r.err = "engine process not running"; return r; }
    drain();
    if (!send("position fen " + fen) || !send("isready")) { r.err = "write to engine failed"; return r; }
    if (!waitFor("readyok", 3000)) { r.err = "no readyok after position (engine hung?)"; r.timeout = true; return r; }
    if (!send("go depth " + std::to_string(depth))) { r.err = "write 'go' failed"; return r; }

    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string line;
    for (;;) {
        int left = msLeft(deadline);
        if (left <= 0) {
            r.timeout = true; r.err = "no bestmove within " + std::to_string(timeoutMs) + " ms";
            send("stop");
            waitFor("bestmove", 1500);   // let it finish so the pipe stays in sync
            return r;
        }
        int rc = readLine(line, left);
        if (rc < 0) { r.err = "engine closed its output (crashed?)"; return r; }
        if (rc == 0) continue;
        if (startsWith(line, "info ")) {
            std::istringstream is(line);
            std::string t; int d = -1; bool bound = false, secondary = false;
            std::string ev, pv;
            while (is >> t) {
                if (t == "depth") is >> d;
                else if (t == "multipv") { int m; is >> m; if (m != 1) secondary = true; }
                else if (t == "lowerbound" || t == "upperbound") bound = true;
                else if (t == "score") {
                    std::string kind; int v = 0; is >> kind >> v;
                    if (stm == 'b') v = -v;                      // report from White's point of view
                    if (kind == "cp") {
                        char buf[24]; int ac = std::abs(v);
                        snprintf(buf, sizeof buf, "%c%d.%02d", v < 0 ? '-' : '+', ac / 100, ac % 100);
                        ev = buf;
                    } else if (kind == "mate") ev = "#" + std::to_string(v);
                } else if (t == "pv") { std::string m; while (is >> m) pv += (pv.empty() ? "" : " ") + m; }
            }
            if (!secondary && !bound && !ev.empty()) { r.eval = ev; if (d > 0) r.depth = d; if (!pv.empty()) r.pv = pv; }
        } else if (startsWith(line, "bestmove")) {
            std::istringstream is(line);
            std::string tag, mv; is >> tag >> mv;
            if (mv == "(none)" || mv == "0000") { r.ok = true; r.err = "no legal moves (checkmate/stalemate)"; return r; }
            if (!validMove(mv)) { r.err = "malformed bestmove '" + line + "'"; return r; }
            r.ok = true; r.bestmove = mv;
            return r;
        }
    }
}
}
