#pragma once

// ===========================================================================
// 自動更新の案内窓（core/UpdateWindow）が使う純ロジック。標準ライブラリだけ（単体テスト: tests/update_ux_test.cpp）。
//   ・GitHub リリース本文（Markdown）→ 案内窓に出す短い行へ（先頭 N 行 + 「ほか M 件」）
//   ・「この版を飛ばす」の判定
//   ・ダウンロードの速度 / 残り時間の見積もりと表示文字列
// ===========================================================================

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace dx12e::updatelogic
{

struct BodyLine
{
    enum class Type { Heading, Bullet, Text };
    Type        type = Type::Text;
    std::string text;   // 太字・コード・リンクなどの記法を外した UTF-8
};

struct BodyView
{
    std::vector<BodyLine> lines;   // 先頭から最大 maxLines 行
    int more = 0;                  // 表示しきれなかった項目数（箇条書きがあればその数、無ければ残りの行数）
};

// Markdown 風の記法を取り除く（**太字** / __太字__ / `コード` / [題](URL) / ![画像](URL) / HTML コメントと <br>）。
std::string StripInlineMarkdown(const std::string& s);

// GitHub リリースの本文を案内窓用に整える。
//   ・最初の「# 見出し」（リリース名の繰り返し）は捨てる。コードブロック / 水平線 / 空行も捨てる。
//   ・「## 見出し」は Heading、「- 」「* 」「+ 」「1. 」は Bullet、それ以外は Text。
//   ・表示の最後が見出しで終わるときは見出しを外す（見出しだけが宙に浮かないように）。
BodyView FormatGithubBody(const std::string& markdown, int maxLines = 6);

// 「この版を飛ばす」の記録（skipped = 飛ばした版）があるとき、latest を案内してよいか。
//   skipped が空 → 案内する / latest が skipped より新しい → 案内する / 同じか古い → 案内しない。
bool ShouldPromptUpdate(const std::string& latest, const std::string& skipped);

// ---- 進捗 ----
// 直近 windowSec 秒の受信量から速度（B/s）を出す。時刻は呼び出し側が渡す（テスト可能）。
class RateTracker
{
public:
    explicit RateTracker(double windowSec = 3.0) : m_window(windowSec) {}
    void   Add(double nowSec, uint64_t doneBytes);
    double BytesPerSec() const;                                   // まだ見積もれないとき 0
    double EtaSeconds(uint64_t done, uint64_t total) const;       // 見積もれないとき < 0
private:
    struct S { double t; uint64_t b; };
    std::deque<S> m_s;
    double        m_window;
};

std::string FormatMB(uint64_t bytes);                  // "12.3"（MiB 1 桁）
std::string FormatSpeed(double bytesPerSec);           // "3.2 MB/s"（0 以下は ""）
std::string FormatEta(double seconds);                 // "約 10 秒" / "約 2 分" （< 0 は ""）
// 「12.3 / 45.6 MB ・ 3.2 MB/s ・ 残り約 10 秒」。total が 0 なら「12.3 MB」だけ。
std::string FormatDownloadDetail(uint64_t done, uint64_t total, double bytesPerSec, double etaSec);

} // namespace dx12e::updatelogic
