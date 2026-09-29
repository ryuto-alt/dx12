#pragma once

// ===== ファジー検索（コマンドパレット / クイックオープン用）=====
// 依存は標準ライブラリだけ（tests/editor_ux_test.cpp が単体で検証する）。
//
// 仕様:
//   ・UTF-8 をコードポイント単位で見る（日本語の「保存」を 1 バイトずつ突き合わせて誤マッチさせない）。
//   ・大文字小文字を無視。全角英数は半角に、カタカナはひらがなに寄せる（「セーブ」と「せーぶ」を同一視）。
//   ・クエリは空白区切りの複数語。**全ての語**が対象に部分列として現れれば一致（順不同）。
//   ・スコア: 連続一致 / 語頭一致 / 文字列先頭一致 / 区切り(空白 - _ / . ( ))直後の一致にボーナス、
//     間が空くほど減点。完全に連続した部分文字列は部分列より必ず高くなるよう重みを付けてある。
//   ・一致しなければ -1。空クエリは 0（全部一致・並びは呼び出し側の MRU 順に任せる）。

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dx12e::fuzzy
{

// UTF-8 → コードポイント列。不正なバイトは U+FFFD にして進める（例外は投げない）。
inline std::vector<uint32_t> DecodeUtf8(std::string_view s, std::vector<uint32_t>* byteOffsets = nullptr)
{
    std::vector<uint32_t> out;
    out.reserve(s.size());
    if (byteOffsets) byteOffsets->clear();
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) { cp = c; }
        else if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu); len = 2; }
        else if ((c >> 4) == 0xE && i + 2 < s.size())
        {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6)
               | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        }
        else if ((c >> 3) == 0x1E && i + 3 < s.size())
        {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12)
               | ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6)
               | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            len = 4;
        }
        out.push_back(cp);
        if (byteOffsets) byteOffsets->push_back(static_cast<uint32_t>(i));
        i += len;
    }
    return out;
}

// 検索用の正規化（1 コードポイント）。大文字→小文字、全角英数→半角、カタカナ→ひらがな。
inline uint32_t Fold(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xFF21 && c <= 0xFF3A) return c - 0xFF21 + 'a';          // Ａ-Ｚ
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 0xFF41 + 'a';          // ａ-ｚ
    if (c >= 0xFF10 && c <= 0xFF19) return c - 0xFF10 + '0';          // ０-９
    if (c >= 0x30A1 && c <= 0x30F6) return c - 0x60;                  // ァ-ヶ → ぁ-ゖ
    if (c == 0x3000) return ' ';                                      // 全角空白
    return c;
}

inline bool IsSeparator(uint32_t c)
{
    return c == ' ' || c == '-' || c == '_' || c == '/' || c == '\\' || c == '.' || c == '(' || c == ')'
        || c == ':' || c == 0x30FB /*・*/ || c == 0xFF08 || c == 0xFF09 || c == 0x3001 || c == 0x3002;
}

// 1 語ぶんのスコア。text は Fold 済みのコードポイント列。
// 戻り値 -1 = 不一致。outPos があれば一致したコードポイントの添字を追記する。
inline int ScoreToken(const std::vector<uint32_t>& tok, const std::vector<uint32_t>& text,
                      std::vector<int>* outPos = nullptr)
{
    if (tok.empty()) return 0;
    if (tok.size() > text.size()) return -1;

    int best = -1;
    std::vector<int> bestPos;

    // 開始位置を全部試して最良の貪欲一致を採る（語が短く対象も数十文字なので十分軽い）。
    for (size_t start = 0; start + tok.size() <= text.size(); ++start)
    {
        if (text[start] != tok[0]) continue;
        std::vector<int> pos;
        pos.reserve(tok.size());
        pos.push_back(static_cast<int>(start));
        size_t ti = 1;
        for (size_t i = start + 1; i < text.size() && ti < tok.size(); ++i)
            if (text[i] == tok[ti]) { pos.push_back(static_cast<int>(i)); ++ti; }
        if (ti != tok.size()) break;   // これ以降の start でも部分列は作れない（後ろが足りない）

        int score = 100;
        int run = 0;
        for (size_t k = 0; k < pos.size(); ++k)
        {
            if (k > 0)
            {
                const int gap = pos[k] - pos[k - 1] - 1;
                if (gap == 0) { ++run; score += 16 + run * 4; }        // 連続一致は強く報いる
                else          { run = 0; score -= (gap > 8 ? 8 : gap) * 2 + 3; }
            }
            const int p = pos[k];
            if (p == 0)                          score += 24;          // 文字列の先頭
            else if (IsSeparator(text[static_cast<size_t>(p) - 1])) score += 14;   // 語頭
        }
        // 対象が短いほど「その語らしい」ので少し加点（長い文の途中に埋もれた一致より上に）
        score -= static_cast<int>(text.size() / 8);
        if (score > best) { best = score; bestPos = pos; }
    }
    if (best >= 0 && outPos) outPos->insert(outPos->end(), bestPos.begin(), bestPos.end());
    return best;
}

// クエリ全体（空白区切りの複数語）と text のスコア。不一致 -1、空クエリ 0。
// outByteOffsets: 一致した文字の「元の UTF-8 の先頭バイト位置」（ハイライト用。重複なし・昇順）。
inline int Score(std::string_view query, std::string_view text, std::vector<uint32_t>* outByteOffsets = nullptr)
{
    if (outByteOffsets) outByteOffsets->clear();

    std::vector<uint32_t> qOff;
    std::vector<uint32_t> q = DecodeUtf8(query, &qOff);
    for (auto& c : q) c = Fold(c);

    // 空白で分割
    std::vector<std::vector<uint32_t>> tokens;
    {
        std::vector<uint32_t> cur;
        for (uint32_t c : q)
        {
            if (c == ' ' || c == '\t') { if (!cur.empty()) { tokens.push_back(std::move(cur)); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) tokens.push_back(std::move(cur));
    }
    if (tokens.empty()) return 0;

    std::vector<uint32_t> tOff;
    std::vector<uint32_t> t = DecodeUtf8(text, &tOff);
    for (auto& c : t) c = Fold(c);

    int total = 0;
    std::vector<int> allPos;
    for (const auto& tok : tokens)
    {
        const int s = ScoreToken(tok, t, &allPos);
        if (s < 0) return -1;
        total += s;
    }
    if (outByteOffsets)
    {
        std::vector<int> uniq = allPos;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        for (int p : uniq) outByteOffsets->push_back(tOff[static_cast<size_t>(p)]);
    }
    return total;
}

} // namespace dx12e::fuzzy
