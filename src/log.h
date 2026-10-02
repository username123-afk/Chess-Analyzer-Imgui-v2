#pragma once
// Tiny thread-safe file logger -> /data/adb/chess_analyzer/run/overlay.log
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
namespace olog {
inline const char* kPath = "/data/adb/chess_analyzer/run/overlay.log";
inline std::mutex& mtx() { static std::mutex m; return m; }
inline FILE*& fp() { static FILE* f = nullptr; return f; }
inline void write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void write(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(mtx());
    if (!fp()) {
        struct stat st;
        if (stat(kPath, &st) == 0 && st.st_size > (1 << 20)) rename(kPath, "/data/adb/chess_analyzer/run/overlay.log.old");
        fp() = fopen(kPath, "a");
        if (!fp()) return;
    }
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    tm t; localtime_r(&ts.tv_sec, &t);
    fprintf(fp(), "%02d-%02d %02d:%02d:%02d.%03d ", t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (int)(ts.tv_nsec / 1000000));
    va_list ap; va_start(ap, fmt);
    vfprintf(fp(), fmt, ap);
    va_end(ap);
    fputc('\n', fp());
    fflush(fp());
}
inline void close() { std::lock_guard<std::mutex> lk(mtx()); if (fp()) { fclose(fp()); fp() = nullptr; } }
}
#define LOG(...) olog::write(__VA_ARGS__)
