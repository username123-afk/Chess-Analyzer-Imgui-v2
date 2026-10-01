#include "chess_core.h"
#include <algorithm>
#include <cstring>
namespace chess {
Board::Board() { std::fill(sq, sq + 64, '.'); }
bool Board::operator==(const Board& o) const { return std::memcmp(sq, o.sq, 64) == 0; }
Board startBoard() {
    Board b; const char* r = "RNBQKBNR"; const char* p = "rnbqkbnr";
    for (int f = 0; f < 8; f++) { b.at(f, 0) = r[f]; b.at(f, 1) = 'P'; b.at(f, 6) = 'p'; b.at(f, 7) = p[f]; }
    return b;
}
bool isWhite(char p) { return p >= 'A' && p <= 'Z'; }
bool validate(const Board& b, bool kings, bool, std::string& why) {
    int wk = 0, bk = 0;
    for (char p : b.sq) {
        if (std::string("PNBRQKpnbrqk.").find(p) == std::string::npos) { why = "invalid piece"; return false; }
        if (p == 'K') wk++;
        if (p == 'k') bk++;
    }
    if (kings && (wk != 1 || bk != 1)) {
        why = "kings: white=" + std::to_string(wk) + " black=" + std::to_string(bk) + " (need 1 each)";
        return false;
    }
    return true;
}
static bool attacked(const Board& b, int f, int r, bool byWhite) {
    auto at = [&](int ff, int rr) -> char { return (ff < 0 || ff > 7 || rr < 0 || rr > 7) ? '.' : b.at(ff, rr); };
    char P = byWhite ? 'P' : 'p', N = byWhite ? 'N' : 'n', B = byWhite ? 'B' : 'b';
    char R = byWhite ? 'R' : 'r', Q = byWhite ? 'Q' : 'q', K = byWhite ? 'K' : 'k';
    int pr = byWhite ? r - 1 : r + 1;
    if (at(f - 1, pr) == P || at(f + 1, pr) == P) return true;
    static const int kn[8][2] = {{1,2},{2,1},{-1,2},{-2,1},{1,-2},{2,-1},{-1,-2},{-2,-1}};
    for (auto& k : kn) if (at(f + k[0], r + k[1]) == N) return true;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            if (!dx && !dy) continue;
            if (at(f + dx, r + dy) == K) return true;
            bool diag = dx && dy;
            for (int k = 1; k < 8; k++) {
                int ff = f + dx * k, rr = r + dy * k;
                if (ff < 0 || ff > 7 || rr < 0 || rr > 7) break;
                char c = b.at(ff, rr);
                if (c == '.') continue;
                if (c == Q || (diag && c == B) || (!diag && c == R)) return true;
                break;
            }
        }
    return false;
}
bool validateFull(const Board& b, char stm, std::string& why) {
    if (!validate(b, true, false, why)) return false;
    int wp = 0, bp = 0, wn = 0, bn = 0, wkf = 0, wkr = 0, bkf = 0, bkr = 0;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            char p = b.at(f, r);
            if (p == '.') continue;
            if (isWhite(p)) wn++; else bn++;
            if (p == 'P') wp++;
            if (p == 'p') bp++;
            if ((p == 'P' || p == 'p') && (r == 0 || r == 7)) { why = "pawn on first/last rank"; return false; }
            if (p == 'K') { wkf = f; wkr = r; }
            if (p == 'k') { bkf = f; bkr = r; }
        }
    if (wp > 8 || bp > 8) { why = "too many pawns"; return false; }
    if (wn > 16 || bn > 16) { why = "too many pieces"; return false; }
    if (std::abs(wkf - bkf) <= 1 && std::abs(wkr - bkr) <= 1) { why = "kings adjacent"; return false; }
    bool whiteToMove = stm == 'w';
    // side NOT to move must not be in check
    if (whiteToMove ? attacked(b, bkf, bkr, true) : attacked(b, wkf, wkr, false)) {
        why = std::string(whiteToMove ? "black" : "white") +
              " king is in check but it is not their turn (wrong 'Side to move' or misread piece)";
        return false;
    }
    return true;
}
std::string sanitizeCastling(const Board& b, const std::string& c) {
    std::string out;
    auto has = [&](char ch) { return c.find(ch) != std::string::npos; };
    if (has('K') && b.at(4, 0) == 'K' && b.at(7, 0) == 'R') out += 'K';
    if (has('Q') && b.at(4, 0) == 'K' && b.at(0, 0) == 'R') out += 'Q';
    if (has('k') && b.at(4, 7) == 'k' && b.at(7, 7) == 'r') out += 'k';
    if (has('q') && b.at(4, 7) == 'k' && b.at(0, 7) == 'r') out += 'q';
    return out.empty() ? "-" : out;
}
std::string sanitizeEp(const Board& b, char stm, const std::string& ep) {
    if (ep.empty() || ep[0] < 'a' || ep[0] > 'h') return "-";
    int f = ep[0] - 'a';
    if (stm == 'w') {
        if (b.at(f, 4) == 'p' && b.at(f, 5) == '.' && b.at(f, 6) == '.') return std::string(1, ep[0]) + "6";
    } else {
        if (b.at(f, 3) == 'P' && b.at(f, 2) == '.' && b.at(f, 1) == '.') return std::string(1, ep[0]) + "3";
    }
    return "-";
}
std::string fen(const Board& b, char side, const std::string& castling, const std::string& ep) {
    std::string s;
    for (int r = 7; r >= 0; r--) {
        int n = 0;
        for (int f = 0; f < 8; f++) {
            char p = b.at(f, r);
            if (p == '.') n++;
            else { if (n) { s += char('0' + n); n = 0; } s += p; }
        }
        if (n) s += char('0' + n);
        if (r) s += '/';
    }
    s += ' '; s += side; s += ' '; s += castling; s += ' '; s += ep; s += " 0 1";
    return s;
}
}
