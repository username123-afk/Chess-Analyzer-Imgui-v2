#pragma once
#include <string>
#include <vector>
namespace chess {
struct Board {
    char sq[64];
    Board();
    char& at(int f, int r) { return sq[r * 8 + f]; }
    char at(int f, int r) const { return sq[r * 8 + f]; }
    bool operator==(const Board& o) const;
};
Board startBoard();
bool isWhite(char p);
// Structural check (piece letters, exactly one king per side).
bool validate(const Board& b, bool requireKings, bool requirePawns, std::string& why);
// Stricter check used before sending a position to Stockfish: piece counts, pawns on
// rank 1/8, adjacent kings, and the side NOT to move must not be in check.
bool validateFull(const Board& b, char stm, std::string& why);
// Drop castling flags / en-passant squares that are impossible on this board.
std::string sanitizeCastling(const Board& b, const std::string& castling);
std::string sanitizeEp(const Board& b, char stm, const std::string& ep);
std::string fen(const Board& b, char side, const std::string& castling = "-", const std::string& ep = "-");
}
