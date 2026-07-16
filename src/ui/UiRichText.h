#pragma once

// UIText のリッチテキスト（インラインタグ）パーサ。UIText.rich=true のときだけ使われる。
// タグはフラット（入れ子なし）で、閉じ忘れは文末まで効く:
//   [c=RRGGBB]...[/c]   スパンの文字色を上書き（16進6桁。αは UIText.color.w を維持）
//   [wave]...[/wave] / [shake]...[/shake] / [rainbow]...[/rainbow]
//                       スパン単位の文字アニメ（charAnim 1/2/3 と同じ動き。
//                       振幅/速度は UIText.charAnimAmount/charAnimSpeed を流用）
// 不正・未知のタグはタグ文字列をそのまま文字として描く（黙って消さない）。
// ヘッダオンリー（inline のみ = リンク不要）なので UISystem.cpp / ScriptEngine.cpp /
// 単体テストのどこからでも使える。依存は <string> / <vector> だけ。

#include <string>
#include <vector>

namespace dx12e
{

// ラン = 装飾が一定の本文断片。b/e は元文字列内のバイト範囲を指す（タグは含まれない =
// タグは幅0・文字数0扱い）。元 std::string より長生きさせないこと。
struct UiRichRun
{
    const char* b = nullptr;
    const char* e = nullptr;
    bool hasColor = false;         // true なら rgb で文字色を上書き
    unsigned int rgb = 0xFFFFFFu;  // 0xRRGGBB（αは本体 UIText.color.w を維持）
    int anim = -1;                 // -1=指定なし（UIText.charAnim を使う） 1=wave 2=shake 3=rainbow
};

namespace richtext_detail
{

// タグ1個の解釈結果
struct UiRichTag
{
    int kind = 0;               // 1=[c=..] 2=[/c] 3=アニメ開始 4=アニメ終了
    int anim = 0;               // kind=3/4 のときのアニメ種（1/2/3）
    unsigned int rgb = 0;       // kind=1 のときの 0xRRGGBB
};

// p（'[' の位置）から妥当なタグを読む。成功なら ']' の次を返し out へ書く。
// 不正・未知のタグは nullptr（呼び出し側が '[' を普通の文字として扱う）。
inline const char* UiRichParseTag(const char* p, const char* end, UiRichTag& out)
{
    // リテラル前方一致（end を越えない）
    auto match = [&](const char* lit) -> const char* {
        const char* q = p;
        for (const char* l = lit; *l != '\0'; ++l, ++q)
            if (q >= end || *q != *l) return nullptr;
        return q;
    };
    if (const char* q = match("[c="))
    {
        // 16進ちょうど6桁 + ']' 以外は不正タグ扱い
        unsigned int v = 0;
        for (int i = 0; i < 6; ++i, ++q)
        {
            if (q >= end) return nullptr;
            const char c = *q;
            unsigned int d = 0;
            if (c >= '0' && c <= '9')      d = static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned int>(c - 'A' + 10);
            else return nullptr;
            v = (v << 4) | d;
        }
        if (q >= end || *q != ']') return nullptr;
        out = {1, 0, v};
        return q + 1;
    }
    if (const char* q = match("[/c]"))       { out = {2, 0, 0}; return q; }
    if (const char* q = match("[wave]"))     { out = {3, 1, 0}; return q; }
    if (const char* q = match("[/wave]"))    { out = {4, 1, 0}; return q; }
    if (const char* q = match("[shake]"))    { out = {3, 2, 0}; return q; }
    if (const char* q = match("[/shake]"))   { out = {4, 2, 0}; return q; }
    if (const char* q = match("[rainbow]"))  { out = {3, 3, 0}; return q; }
    if (const char* q = match("[/rainbow]")) { out = {4, 3, 0}; return q; }
    return nullptr;
}

} // namespace richtext_detail

// text をラン列へ分解する（out は先頭でクリア）。UI 文字列は短いので毎フレーム呼んでよい。
// 呼び出し側で static vector を使い回せば再確保も起きない。
// UTF-8 の継続バイトは 0x80 以上なので '[' のバイト走査はマルチバイト文字を誤爆しない。
inline void ParseUiRichText(const std::string& text, std::vector<UiRichRun>& out)
{
    out.clear();
    const char* p = text.c_str();
    const char* end = p + text.size();
    const char* runBegin = p;
    bool hasColor = false;
    unsigned int rgb = 0xFFFFFFu;
    int anim = -1;
    auto flush = [&](const char* runEnd) {
        if (runEnd > runBegin)
            out.push_back({runBegin, runEnd, hasColor, rgb, anim});
    };
    while (p < end)
    {
        if (*p == '[')
        {
            richtext_detail::UiRichTag tag;
            if (const char* after = richtext_detail::UiRichParseTag(p, end, tag))
            {
                flush(p);
                switch (tag.kind)
                {
                case 1:  hasColor = true; rgb = tag.rgb; break;
                case 2:  hasColor = false;               break;
                case 3:  anim = tag.anim;                break;
                default: anim = -1;                      break; // 閉じタグは種別不問で解除（フラット前提）
                }
                p = runBegin = after;
                continue;
            }
        }
        ++p;   // 不正・未知のタグ/普通の文字はランに含めたまま進む
    }
    flush(end);
}

// 値返し版（テスト等の楽な呼び方用）
inline std::vector<UiRichRun> ParseUiRichText(const std::string& text)
{
    std::vector<UiRichRun> runs;
    ParseUiRichText(text, runs);
    return runs;
}

// 一時 string は禁止（ランは元文字列内を指す = 呼び出し直後にダングリングする）
void ParseUiRichText(std::string&&, std::vector<UiRichRun>&) = delete;
std::vector<UiRichRun> ParseUiRichText(std::string&&) = delete;

// タグ除去後の UTF-8 コードポイント数（\n も1と数える）。タイプライターの総文字数・
// 完了判定（isUiTypewriterDone）用。描画側の数え方と一致させること。
inline int UiRichStrippedCodepoints(const std::string& text)
{
    const char* p = text.c_str();
    const char* end = p + text.size();
    int count = 0;
    while (p < end)
    {
        if (*p == '[')
        {
            richtext_detail::UiRichTag tag;
            if (const char* after = richtext_detail::UiRichParseTag(p, end, tag))
            {
                p = after;
                continue;
            }
        }
        const auto c = static_cast<unsigned char>(*p);
        p += (c < 0xC0) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (p > end) p = end;   // 末尾の壊れた UTF-8 でも読み越さない
        ++count;
    }
    return count;
}

} // namespace dx12e
