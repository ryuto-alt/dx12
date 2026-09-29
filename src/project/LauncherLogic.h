#pragma once

// ===========================================================================
// プロジェクトランチャーの純ロジック（ヘッダオンリー・std だけ。ImGui / Win32 / GPU / JSON に依存しない）
// ---------------------------------------------------------------------------
// 画面（editor/LauncherScreen.cpp）と単体テスト（tests/launcher_logic_test.cpp）が【同じ関数】を通る。
//   ・テンプレート登録表        : 新しいテンプレートは Templates() に 1 行足すだけ（+ ProjectTemplates.cpp のファイル表 + 画像）
//   ・名前 / 保存場所の検証     : 作成先パスをその場で検証する（既存フォルダ / 書き込み権限 / 日本語・全角スペース等の警告 …）
//   ・最近のプロジェクトの整形  : 並び（ピン留め優先 → 新しい順）/ 検索 / 追加・削除・ピン留め / 相対時刻
//   ・サムネイル                : 保存先パスの決定 / 中央 16:9 切り出し + 箱フィルタ縮小（BGRA8）
//
// 文字列は UTF-8。時刻は epoch 秒（int64）。ファイルシステムの問い合わせは FsProbe 経由（テストで差し替えられる）。
// ===========================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "editor/EditorIcons.h"   // ICON_*（マクロだけ。テンプレ表のグリフ）

namespace dx12e::launcher
{

// ---------------------------------------------------------------- 文字列の小道具（UTF-8）

inline std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// U+3000（全角スペース）= E3 80 80。
inline bool ContainsFullWidthSpace(const std::string& s)
{
    return s.find("　") != std::string::npos;
}
inline bool ContainsNonAscii(const std::string& s)
{
    for (unsigned char c : s) if (c >= 0x80) return true;
    return false;
}
inline size_t Utf8Length(const std::string& s)
{
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
// UTF-8 の先頭 n コードポイント分のバイト長。
inline size_t Utf8PrefixBytes(const std::string& s, size_t n)
{
    size_t i = 0, cp = 0;
    while (i < s.size() && cp < n)
    {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        ++cp;
    }
    return i;
}
// 末尾 n コードポイント分の開始バイト位置。
inline size_t Utf8SuffixStart(const std::string& s, size_t n)
{
    size_t total = Utf8Length(s);
    if (n >= total) return 0;
    return Utf8PrefixBytes(s, total - n);
}

inline char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline std::string AsciiLowerStr(const std::string& s)
{
    std::string r = s;
    for (char& c : r) c = AsciiLower(c);
    return r;
}

// パスの比較キー: 区切りを '/' に揃え、末尾の区切りを落とし、ASCII を小文字化（Windows は大文字小文字を区別しない）。
inline std::string PathKey(const std::string& p)
{
    std::string r = p;
    for (char& c : r) { if (c == '\\') c = '/'; c = AsciiLower(c); }
    while (r.size() > 1 && r.back() == '/' && !(r.size() == 3 && r[1] == ':')) r.pop_back();
    return r;
}

// パスの最後の要素（区切りは / と \ の両方）。
inline std::string PathLeaf(const std::string& p)
{
    std::string r = p;
    while (!r.empty() && (r.back() == '/' || r.back() == '\\')) r.pop_back();
    const size_t k = r.find_last_of("/\\");
    return k == std::string::npos ? r : r.substr(k + 1);
}

// パスの結合（区切りは '\\' に揃える。Windows の表示用）。
inline std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    std::string r = a;
    if (r.back() != '\\' && r.back() != '/') r += '\\';
    r += b;
    return r;
}

// 中央省略: 長いパスを maxCodepoints 文字以内へ。先頭（ドライブ）と末尾（フォルダ名）を残す。
inline std::string MiddleEllipsis(const std::string& s, size_t maxCodepoints)
{
    const size_t n = Utf8Length(s);
    if (n <= maxCodepoints) return s;
    if (maxCodepoints <= 1) return "…";
    const size_t keep = maxCodepoints - 1;
    const size_t head = (keep + 1) / 3;              // 先頭は少なめ（ドライブ名が分かれば足りる）
    const size_t tail = keep - head;
    const size_t hb = Utf8PrefixBytes(s, head);
    const size_t ts = Utf8SuffixStart(s, tail);
    return s.substr(0, hb) + "…" + s.substr(ts);
}

// ---------------------------------------------------------------- 時刻の整形

// 1970-01-01 からの日数 → 年月日（グレゴリオ暦の純計算。タイムゾーンは tzOffsetSec で与える）。
inline void CivilFromDays(int64_t z, int& y, int& m, int& d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy  = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp  = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

inline std::string FormatDate(int64_t epochSec, int tzOffsetSec)
{
    const int64_t local = epochSec + tzOffsetSec;
    const int64_t days  = (local >= 0 ? local : local - 86399) / 86400;
    int y, m, d;
    CivilFromDays(days, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d", y, m, d);
    return buf;
}

// 「たった今」「3 分前」「2 時間前」「昨日」「5 日前」「2026/09/01」。then が 0 以下 = 記録なし。
inline std::string FormatRelativeTime(int64_t now, int64_t then, int tzOffsetSec = 0)
{
    if (then <= 0) return "";
    const int64_t dt = now - then;
    char buf[48];
    if (dt < 60)                 return "たった今";               // たった今
    if (dt < 3600)               { std::snprintf(buf, sizeof(buf), "%lld 分前", static_cast<long long>(dt / 60)); return buf; }        // 分前
    if (dt < 86400)              { std::snprintf(buf, sizeof(buf), "%lld 時間前", static_cast<long long>(dt / 3600)); return buf; } // 時間前
    if (dt < 2 * 86400)          return "昨日";                                          // 昨日
    if (dt < 30LL * 86400)       { std::snprintf(buf, sizeof(buf), "%lld 日前", static_cast<long long>(dt / 86400)); return buf; }       // 日前
    return FormatDate(then, tzOffsetSec);
}

// ---------------------------------------------------------------- 検証の結果型

enum class Severity { Ok = 0, Info = 1, Warn = 2, Error = 3 };

struct Issue
{
    Severity    sev = Severity::Ok;
    std::string code;      // "name.empty" など（テスト / UI の分岐用。文言を変えても壊れない）
    std::string message;   // 標準語の画面文言
};

struct Validation
{
    std::vector<Issue> issues;

    bool Has(Severity s) const { for (const auto& i : issues) if (i.sev == s) return true; return false; }
    bool HasError() const { return Has(Severity::Error); }
    bool HasWarn()  const { return Has(Severity::Warn); }
    bool HasCode(const std::string& code) const { for (const auto& i : issues) if (i.code == code) return true; return false; }
    Severity Worst() const
    {
        Severity w = Severity::Ok;
        for (const auto& i : issues) if (static_cast<int>(i.sev) > static_cast<int>(w)) w = i.sev;
        return w;
    }
    // 重い順に並べる（Error → Warn → Info）。同じ重さは入れた順のまま。
    std::vector<Issue> Sorted() const
    {
        std::vector<Issue> r = issues;
        std::stable_sort(r.begin(), r.end(), [](const Issue& a, const Issue& b) { return static_cast<int>(a.sev) > static_cast<int>(b.sev); });
        return r;
    }
    void Add(Severity s, const char* code, const std::string& msg) { issues.push_back({ s, code, msg }); }
    void Append(const Validation& o) { issues.insert(issues.end(), o.issues.begin(), o.issues.end()); }
};

// ---------------------------------------------------------------- ファイルシステムの問い合わせ（差し替え可能）

struct FsProbe
{
    std::function<bool(const std::string&)> exists;         // 何かある
    std::function<bool(const std::string&)> isDirectory;
    std::function<bool(const std::string&)> isEmptyDir;     // ディレクトリで中身が空
    std::function<bool(const std::string&)> canWriteDir;    // そのディレクトリへ書ける（存在するディレクトリだけ問う）
    std::function<bool(const std::string&)> hasProjectFile; // .dx12proj を直下に持つ
};

inline std::filesystem::path PathFromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
inline std::string PathToUtf8(const std::filesystem::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// 実ファイルシステム版。canWriteDir は「一時ファイルを作って消す」で確かめる（ACL を正しく反映する唯一の方法）。
inline FsProbe RealFs()
{
    namespace fs = std::filesystem;
    FsProbe p;
    p.exists = [](const std::string& s) { std::error_code ec; return fs::exists(PathFromUtf8(s), ec); };
    p.isDirectory = [](const std::string& s) { std::error_code ec; return fs::is_directory(PathFromUtf8(s), ec); };
    p.isEmptyDir = [](const std::string& s)
    {
        std::error_code ec;
        const fs::path d = PathFromUtf8(s);
        if (!fs::is_directory(d, ec)) return false;
        return fs::directory_iterator(d, ec) == fs::directory_iterator();
    };
    p.canWriteDir = [](const std::string& s)
    {
        std::error_code ec;
        const fs::path d = PathFromUtf8(s);
        if (!fs::is_directory(d, ec)) return false;
        const fs::path probe = d / (".uno_write_probe_" + std::to_string(static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()) & 0xFFFFFF) + ".tmp");
        {
            std::ofstream f(probe, std::ios::binary);
            if (!f) return false;
            f << 'x';
            if (!f) return false;
        }
        fs::remove(probe, ec);
        return true;
    };
    p.hasProjectFile = [](const std::string& s)
    {
        std::error_code ec;
        const fs::path d = PathFromUtf8(s);
        if (!fs::is_directory(d, ec)) return false;
        for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".dx12proj") return true;
        return false;
    };
    return p;
}

// ---------------------------------------------------------------- 名前の検証

// Windows のフォルダ名に使えない文字（重複なし）。制御文字は「(制御文字)」と 1 回だけ足す。
inline std::string IllegalNameChars(const std::string& s)
{
    std::string found;
    bool ctrl = false;
    for (unsigned char c : s)
    {
        if (c < 0x20) { ctrl = true; continue; }
        if (std::strchr("<>:\"/\\|?*", c) != nullptr && found.find(static_cast<char>(c)) == std::string::npos)
            found += static_cast<char>(c);
    }
    if (ctrl) found += "(制御文字)";
    return found;
}

// CON / PRN / AUX / NUL / COM1-9 / LPT1-9（拡張子付きでも予約）。
inline bool IsReservedDeviceName(const std::string& name)
{
    std::string base = AsciiLowerStr(name);
    const size_t dot = base.find('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
    if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0)
        && base[3] >= '1' && base[3] <= '9') return true;
    return false;
}

constexpr size_t kNameMaxCodepoints = 64;    // これを超えたら警告
constexpr size_t kNameHardMaxBytes  = 120;   // これを超えたらエラー（assets/ 以下の深いパスで 260 を割りやすい）

inline Validation ValidateProjectName(const std::string& rawName)
{
    Validation v;
    const std::string name = rawName;
    if (Trim(name).empty())
    {
        v.Add(Severity::Error, "name.empty", "プロジェクト名を入力してください");
        return v;
    }
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.')
        v.Add(Severity::Error, "name.edge", "名前の先頭・末尾には空白やピリオドを使えません");
    const std::string bad = IllegalNameChars(name);
    if (!bad.empty())
        v.Add(Severity::Error, "name.chars", std::string("名前に使えない文字があります: ") + bad);   // 名前に使えない文字があります:
    if (IsReservedDeviceName(name))
        v.Add(Severity::Error, "name.reserved", "この名前は Windows の予約名なので使えません");
    if (name.size() > kNameHardMaxBytes)
        v.Add(Severity::Error, "name.too_long", "名前が長すぎます（パスの上限を超えやすくなります）");
    else if (Utf8Length(name) > kNameMaxCodepoints)
        v.Add(Severity::Warn, "name.long", "名前が長めです（" "64 文字以内を推奨）");   // 名前が長めです（64 文字以内を推奨）
    if (!name.empty() && name.front() == '.')
        v.Add(Severity::Warn, "name.dot_start", "先頭がピリオドの名前は隠しフォルダ扱いになりやすいです");
    if (ContainsFullWidthSpace(name))
        v.Add(Severity::Warn, "name.fullwidth_space", "全角スペースが含まれています。半角の文字へすると安全です");
    else if (ContainsNonAscii(name))
        v.Add(Severity::Warn, "name.non_ascii", "日本語などの文字が含まれています。外部ツール（Git・ビルド等）で問題が出ることがあります");
    return v;
}

// ---------------------------------------------------------------- 保存場所の検証

inline bool IsAbsoluteWindowsPath(const std::string& p)
{
    if (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/'))
        return true;
    if (p.size() >= 3 && p[0] == '\\' && p[1] == '\\' && p[2] != '\\') return true;   // UNC
    return false;
}

// 親ディレクトリのうち、実在する最も深いもの（無ければ空）。
inline std::string NearestExistingAncestor(const std::string& path, const FsProbe& fs)
{
    std::string cur = path;
    for (int guard = 0; guard < 64; ++guard)
    {
        while (!cur.empty() && (cur.back() == '\\' || cur.back() == '/') && cur.size() > 3) cur.pop_back();
        if (cur.empty()) return "";
        if (fs.exists(cur)) return cur;
        if (cur.size() <= 3) return "";                        // ドライブ直下まで来ても無い = ドライブが無い
        const size_t k = cur.find_last_of("/\\");
        if (k == std::string::npos) return "";
        if (k == 2) { cur = cur.substr(0, 3); continue; }       // "C:\xxx" → "C:\"
        if (k < 2) return "";
        cur = cur.substr(0, k);
    }
    return "";
}

inline bool PathHasSegmentWithEdgeSpace(const std::string& p)
{
    size_t start = 0;
    while (start <= p.size())
    {
        size_t e = p.find_first_of("/\\", start);
        if (e == std::string::npos) e = p.size();
        const std::string seg = p.substr(start, e - start);
        if (!seg.empty() && seg != "." && seg != ".." && (seg.front() == ' ' || seg.back() == ' ' || (seg.back() == '.' && seg.size() > 1)))
            return true;
        start = e + 1;
    }
    return false;
}

inline bool ContainsCaseInsensitive(const std::string& hay, const char* needle)
{
    return AsciiLowerStr(hay).find(AsciiLowerStr(needle)) != std::string::npos;
}

// 保存場所（親フォルダ）と名前から作る「作成先 = <保存場所>\<名前>」を検証する。
//   rawParent  … 保存場所（UTF-8）
//   name       … プロジェクト名（検証済みでなくてもよい。ここでは名前自体は ValidateProjectName に任せる）
//   fs         … ファイルシステム
//   forClone   … true のとき文言を「クローン先」向けにする（既に .dx12proj がある場合の扱いなど）
inline Validation ValidateLocation(const std::string& rawParent, const std::string& name, const FsProbe& fs, bool forClone = false)
{
    Validation v;
    const std::string parent = Trim(rawParent);
    if (parent.empty())
    {
        v.Add(Severity::Error, "loc.empty", "保存場所を指定してください");
        return v;
    }
    if (!IsAbsoluteWindowsPath(parent))
    {
        v.Add(Severity::Error, "loc.relative", "保存場所はドライブ名付きのフルパスで指定してください（例: D:\\Projects）");   // 保存場所はドライブ名付きのフルパスで指定してください（例: D:\Projects）
        return v;
    }
    // ドライブ文字の直後の ':' 以外にパス中の禁則文字がないか（<>"|?* と制御文字）
    {
        std::string bad;
        for (size_t i = 0; i < parent.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(parent[i]);
            if (c < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
                if (bad.find(static_cast<char>(c)) == std::string::npos && c >= 0x20) bad += static_cast<char>(c);
            if (c == ':' && i != 1) if (bad.find(':') == std::string::npos) bad += ':';
        }
        if (!bad.empty())
            v.Add(Severity::Error, "loc.chars", std::string("保存場所に使えない文字があります: ") + bad);   // 保存場所に使えない文字があります:
    }
    if (PathHasSegmentWithEdgeSpace(parent))
        v.Add(Severity::Error, "loc.segment_edge", "保存場所のフォルダ名の先頭・末尾に空白やピリオドがあります");

    if (v.HasError()) return v;   // 以降は実在確認が意味を持つ形のときだけ

    // 実在の確認
    const std::string target = JoinPath(parent, name);
    const bool parentExists = fs.exists(parent);
    if (parentExists && !fs.isDirectory(parent))
    {
        v.Add(Severity::Error, "loc.not_dir", "保存場所に同名のファイルがあり、フォルダとして使えません");
        return v;
    }
    const std::string anc = parentExists ? parent : NearestExistingAncestor(parent, fs);
    if (anc.empty())
    {
        v.Add(Severity::Error, "loc.no_root", "保存場所のドライブや上位フォルダが見つかりません");
        return v;
    }
    if (!parentExists)
        v.Add(Severity::Info, "loc.will_create_parents", "保存場所のフォルダはこのまま作成されます");
    if (!fs.canWriteDir(anc))
        v.Add(Severity::Error, "loc.not_writable", std::string("この場所には書き込めません（書き込み権限がありません）"));

    // 作成先
    if (!Trim(name).empty())
    {
        if (fs.exists(target))
        {
            if (!fs.isDirectory(target))
                v.Add(Severity::Error, "target.is_file", "同名のファイルがすでにあります");
            else if (fs.hasProjectFile(target))
                v.Add(Severity::Error, forClone ? "target.exists_nonempty" : "target.already_project",
                      forClone ? "クローン先にすでにUno Engine のプロジェクトがあります"
                               : "その名前のプロジェクトがすでにあります。「開く」から開いてください");
            else if (!fs.isEmptyDir(target))
                v.Add(Severity::Error, "target.exists_nonempty", "同名のフォルダがすでにあり、中身があります。別の名前を使ってください");
            else
                v.Add(Severity::Info, "target.exists_empty", "同名の空のフォルダがあります。その中へ作成します");
        }
    }

    // 警告: 場所の性質
    if (ContainsFullWidthSpace(parent))
        v.Add(Severity::Warn, "path.fullwidth_space", "保存場所に全角スペースが含まれています。外部ツールで問題になることがあります");
    else if (ContainsNonAscii(parent))
        v.Add(Severity::Warn, "path.non_ascii", "保存場所に日本語などの文字が含まれています。外部ツール（Git・ビルド等）で問題が出ることがあります");

    // パスの長さ: 作成先 + 内部の深い相対パス（assets/audio/bgm/… で 40 ほど）+ 余裕
    {
        const size_t total = target.size();   // バイト数（UTF-8）。日本語は実際の UTF-16 長より大きく出る＝安全側
        if (total > 240)
            v.Add(Severity::Error, "path.too_long", "パスが長すぎます（Windows の上限 260 文字を超えます）。浅い場所を指定してください");
        else if (total > 180)
            v.Add(Severity::Warn, "path.long", "パスが長めです。深いフォルダのアセットで Windows の上限（260 文字）に当たりやすくなります");
    }
    if (ContainsCaseInsensitive(parent + "\\", "\\Program Files\\") || ContainsCaseInsensitive(parent + "\\", "\\Program Files (x86)\\")
        || ContainsCaseInsensitive(parent + "\\", "\\Windows\\"))
        v.Add(Severity::Warn, "loc.protected", "システムフォルダの中です。ユーザーのドキュメント等をおすすめします");
    if (ContainsCaseInsensitive(parent, "\\OneDrive"))
        v.Add(Severity::Warn, "loc.onedrive", "OneDrive の同期フォルダの中です。大量のファイルの同期で遅くなることがあります");
    if (parentExists && fs.hasProjectFile(parent))
        v.Add(Severity::Warn, "loc.inside_project", "保存場所が別のプロジェクトの中です。入れ子になるので、場所を変えることをおすすめします");
    return v;
}

// 最近のプロジェクトとの重複（同名で別の場所）。
struct RecentRecord
{
    std::string name;
    std::string path;          // プロジェクトルート（UTF-8）
    int64_t     lastOpened = 0;   // epoch 秒。0 = 記録なし（旧形式の recent.json）
    bool        pinned = false;
};

inline Validation ValidateDuplicateName(const std::string& name, const std::string& targetPath, const std::vector<RecentRecord>& recents)
{
    Validation v;
    const std::string keyName = AsciiLowerStr(Trim(name));
    const std::string keyPath = PathKey(targetPath);
    for (const auto& r : recents)
    {
        if (AsciiLowerStr(r.name) == keyName && PathKey(r.path) != keyPath)
        {
            v.Add(Severity::Warn, "dup.recent_name", "同じ名前のプロジェクトが最近の一覧にあります。区別しやすい名前をおすすめします");
            break;
        }
    }
    return v;
}

// 新規作成フォーム全体の検証（名前 + 保存場所 + 重複）。
inline Validation ValidateNewProject(const std::string& name, const std::string& parent, const FsProbe& fs,
                                     const std::vector<RecentRecord>& recents)
{
    Validation v = ValidateProjectName(name);
    const Validation loc = ValidateLocation(parent, Trim(name), fs, false);
    // 名前が空のときは保存場所側の「作成先」系の警告だけ出しても意味が薄いので、そのまま足す。
    v.Append(loc);
    if (!v.HasError() && !Trim(name).empty())
        v.Append(ValidateDuplicateName(name, JoinPath(Trim(parent), Trim(name)), recents));
    return v;
}

// 既定の名前: base, base2, base3 … のうち、保存場所に無いもの。
inline std::string SuggestUniqueName(const std::string& parent, const std::string& base, const FsProbe& fs)
{
    if (parent.empty() || !fs.exists(parent)) return base;
    if (!fs.exists(JoinPath(parent, base))) return base;
    for (int i = 2; i < 1000; ++i)
    {
        const std::string cand = base + std::to_string(i);
        if (!fs.exists(JoinPath(parent, cand))) return cand;
    }
    return base;
}

// ---------------------------------------------------------------- Git URL

inline std::string RepoNameFromGitUrl(const std::string& urlIn)
{
    std::string u = Trim(urlIn);
    while (!u.empty() && (u.back() == '/' || u.back() == '\\')) u.pop_back();
    if (u.size() > 4 && AsciiLowerStr(u.substr(u.size() - 4)) == ".git") u.resize(u.size() - 4);
    const size_t k = u.find_last_of("/:\\");
    return k == std::string::npos ? u : u.substr(k + 1);
}

inline Validation ValidateGitUrl(const std::string& urlIn)
{
    Validation v;
    const std::string u = Trim(urlIn);
    if (u.empty())
    {
        v.Add(Severity::Error, "url.empty", "リポジトリの URL を入力してください");
        return v;
    }
    if (u.find_first_of(" \t\r\n") != std::string::npos)
    {
        v.Add(Severity::Error, "url.space", "URL に空白や改行が含まれています");
        return v;
    }
    const std::string l = AsciiLowerStr(u);
    const bool okScheme = l.rfind("https://", 0) == 0 || l.rfind("http://", 0) == 0 || l.rfind("ssh://", 0) == 0
                       || l.rfind("git@", 0) == 0 || l.rfind("git://", 0) == 0;
    if (!okScheme)
    {
        v.Add(Severity::Error, "url.scheme", "URL は https://、ssh://、git@ のいずれかで始めてください");
        return v;
    }
    if (RepoNameFromGitUrl(u).empty())
        v.Add(Severity::Error, "url.no_repo", "URL からリポジトリ名を読み取れません");
    return v;
}

// ---------------------------------------------------------------- 最近のプロジェクト（並び・検索・編集）

constexpr size_t kMaxUnpinnedRecents = 24;

// ピン留め優先 → 新しい順。記録なし（lastOpened=0）は元の並びを保ったまま最後尾側へ。
inline void SortRecents(std::vector<RecentRecord>& v)
{
    std::vector<std::pair<size_t, RecentRecord>> idx;
    idx.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) idx.emplace_back(i, v[i]);
    std::stable_sort(idx.begin(), idx.end(), [](const auto& a, const auto& b)
    {
        if (a.second.pinned != b.second.pinned) return a.second.pinned;
        if (a.second.lastOpened != b.second.lastOpened) return a.second.lastOpened > b.second.lastOpened;
        return a.first < b.first;
    });
    for (size_t i = 0; i < v.size(); ++i) v[i] = idx[i].second;
}

// 開いた / 作った: 先頭へ・時刻更新・ピン状態は保つ・ピン無しは上限で切る。
inline void UpsertRecent(std::vector<RecentRecord>& v, const std::string& name, const std::string& path, int64_t now)
{
    bool pinned = false;
    const std::string key = PathKey(path);
    for (auto it = v.begin(); it != v.end();)
    {
        if (PathKey(it->path) == key) { pinned = it->pinned; it = v.erase(it); }
        else ++it;
    }
    RecentRecord r;
    r.name = name; r.path = path; r.lastOpened = now; r.pinned = pinned;
    v.insert(v.begin(), r);
    SortRecents(v);
    size_t unpinned = 0;
    for (auto it = v.begin(); it != v.end();)
    {
        if (!it->pinned && ++unpinned > kMaxUnpinnedRecents) it = v.erase(it);
        else ++it;
    }
}

inline bool SetPinned(std::vector<RecentRecord>& v, const std::string& path, bool pinned)
{
    const std::string key = PathKey(path);
    for (auto& r : v)
        if (PathKey(r.path) == key) { r.pinned = pinned; SortRecents(v); return true; }
    return false;
}

inline bool RemoveRecent(std::vector<RecentRecord>& v, const std::string& path)
{
    const std::string key = PathKey(path);
    for (auto it = v.begin(); it != v.end(); ++it)
        if (PathKey(it->path) == key) { v.erase(it); return true; }
    return false;
}

// 大文字小文字（ASCII）と区切り（\ と /）を揃えた比較用文字列。
inline std::string SlashLower(const std::string& t)
{
    std::string r = AsciiLowerStr(t);
    for (char& c : r) if (c == '\\') c = '/';
    return r;
}

// 検索: 空白区切りの語が【全部】名前かパスに含まれる（ASCII は大文字小文字を区別しない）。戻りは v への添字。
inline std::vector<int> FilterRecents(const std::vector<RecentRecord>& v, const std::string& query)
{
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : query)
        {
            if (c == ' ' || c == '\t') { if (!cur.empty()) { tokens.push_back(SlashLower(cur)); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) tokens.push_back(SlashLower(cur));
    }
    std::vector<int> out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (tokens.empty()) { out.push_back(static_cast<int>(i)); continue; }
        const std::string hay = SlashLower(v[i].name) + "\n" + SlashLower(v[i].path);
        bool all = true;
        for (const auto& t : tokens) if (hay.find(t) == std::string::npos) { all = false; break; }
        if (all) out.push_back(static_cast<int>(i));
    }
    return out;
}

// ---------------------------------------------------------------- サムネイル

// 保存先: <プロジェクトルート>/.dx12/thumbnail.png（.dx12/ は既にセーブ・バックアップ用の「ユーザー領域」）。
inline std::string ThumbnailRelPath() { return ".dx12/thumbnail.png"; }
inline std::string ThumbnailPath(const std::string& projectRoot)
{
    std::string r = projectRoot;
    while (!r.empty() && (r.back() == '/' || r.back() == '\\')) r.pop_back();
    return r + "/" + ThumbnailRelPath();
}

constexpr int kThumbW = 640;
constexpr int kThumbH = 360;

struct ThumbCrop { int x = 0, y = 0, w = 0, h = 0; };

// 元画像（srcW × srcH）から 16:9 を中央で切り出す。
inline ThumbCrop CenterCrop16x9(int srcW, int srcH)
{
    ThumbCrop c;
    if (srcW <= 0 || srcH <= 0) return c;
    // 16:9 に合わせる（整数）
    if (static_cast<int64_t>(srcW) * 9 >= static_cast<int64_t>(srcH) * 16)
    {
        c.h = srcH; c.w = static_cast<int>(static_cast<int64_t>(srcH) * 16 / 9);
    }
    else
    {
        c.w = srcW; c.h = static_cast<int>(static_cast<int64_t>(srcW) * 9 / 16);
    }
    c.x = (srcW - c.w) / 2;
    c.y = (srcH - c.h) / 2;
    return c;
}

// BGRA8 の切り出し + 箱フィルタ縮小（dst が src より大きい場合は最近傍で拡大しない＝呼び出し側が dst を選ぶ）。
inline std::vector<uint8_t> DownscaleBgra(const uint8_t* src, int srcW, int srcH, int srcPitchBytes,
                                          const ThumbCrop& crop, int dstW, int dstH)
{
    std::vector<uint8_t> out(static_cast<size_t>(dstW) * dstH * 4, 0);
    if (!src || dstW <= 0 || dstH <= 0 || crop.w <= 0 || crop.h <= 0) return out;
    for (int y = 0; y < dstH; ++y)
    {
        const int sy0 = crop.y + static_cast<int>(static_cast<int64_t>(y) * crop.h / dstH);
        int sy1 = crop.y + static_cast<int>(static_cast<int64_t>(y + 1) * crop.h / dstH);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        sy1 = (std::min)(sy1, srcH);
        for (int x = 0; x < dstW; ++x)
        {
            const int sx0 = crop.x + static_cast<int>(static_cast<int64_t>(x) * crop.w / dstW);
            int sx1 = crop.x + static_cast<int>(static_cast<int64_t>(x + 1) * crop.w / dstW);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            sx1 = (std::min)(sx1, srcW);
            unsigned sum[4] = { 0, 0, 0, 0 };
            unsigned n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
            {
                const uint8_t* row = src + static_cast<size_t>(sy) * srcPitchBytes;
                for (int sx = sx0; sx < sx1; ++sx)
                {
                    const uint8_t* p = row + static_cast<size_t>(sx) * 4;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
                    ++n;
                }
            }
            uint8_t* o = &out[(static_cast<size_t>(y) * dstW + x) * 4];
            if (n == 0) continue;
            o[0] = static_cast<uint8_t>(sum[0] / n); o[1] = static_cast<uint8_t>(sum[1] / n);
            o[2] = static_cast<uint8_t>(sum[2] / n); o[3] = 255;
        }
    }
    return out;
}

// ほぼ真っ黒 / 単色の画（暗転中・ロード中に撮った失敗）は保存しない。平均輝度と分散で判定。
inline bool ThumbnailLooksBlank(const std::vector<uint8_t>& bgra, int w, int h)
{
    if (bgra.size() < static_cast<size_t>(w) * h * 4 || w <= 0 || h <= 0) return true;
    double sum = 0, sum2 = 0;
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i)
    {
        const double l = 0.2126 * bgra[i * 4 + 2] + 0.7152 * bgra[i * 4 + 1] + 0.0722 * bgra[i * 4 + 0];
        sum += l; sum2 += l * l;
    }
    const double mean = sum / n;
    const double var = sum2 / n - mean * mean;
    return mean < 2.0 || var < 2.0;
}

// ---------------------------------------------------------------- ニュース（更新内容）の整形

// Version.cpp の kWhatsNewBody（手で折り返した平文）を、画面で組める塊へ分ける。
//   1 行目の見出し / 「■」で始まる節 / 字下げの続き行（折り返しなので 1 段落へ結合）/ 「・」の箇条書き。
struct NewsBlock
{
    enum class Kind { Headline, Section, Paragraph, Bullet };
    Kind        kind = Kind::Paragraph;
    std::string text;
};

inline std::vector<NewsBlock> ParseNewsBody(const std::string& body)
{
    std::vector<NewsBlock> out;
    std::string para;
    auto flush = [&]()
    {
        if (!para.empty()) { out.push_back({ NewsBlock::Kind::Paragraph, para }); para.clear(); }
    };
    auto isAsciiWord = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto startsWith = [](const std::string& s, const char* pre) { return s.compare(0, std::strlen(pre), pre) == 0; };

    size_t pos = 0;
    bool first = true;
    while (pos <= body.size())
    {
        size_t e = body.find('\n', pos);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (Trim(line).empty()) { flush(); continue; }
        const bool indented = line.size() >= 2 && line[0] == ' ' && line[1] == ' ';
        const std::string t = Trim(line);

        if (startsWith(t, "■"))
        {
            flush();
            out.push_back({ NewsBlock::Kind::Section, Trim(t.substr(std::strlen("■"))) });
        }
        else if (startsWith(t, "・") || startsWith(t, "- "))
        {
            flush();
            const size_t skip = startsWith(t, "・") ? std::strlen("・") : 2;
            out.push_back({ NewsBlock::Kind::Bullet, Trim(t.substr(skip)) });
        }
        else if (indented && !para.empty())
        {
            // 手動の折り返しの続き行。英数字どうしがくっつくときだけ空白を補う。
            if (isAsciiWord(para.back()) && isAsciiWord(t.front())) para += ' ';
            para += t;
        }
        else if (first)
        {
            out.push_back({ NewsBlock::Kind::Headline, t });
        }
        else
        {
            flush();
            para = t;
        }
        first = false;
    }
    flush();
    return out;
}

// ---------------------------------------------------------------- テンプレート登録表（データ駆動）

struct TemplateDef
{
    const char* id;            // ProjectTemplates.cpp の GetFiles() と同じ ID
    const char* name;          // カードの見出し
    const char* subtitle;      // ジャンル（小さく）
    const char* tagline;       // カードの 1 行説明
    const char* description;   // 右パネルの説明（数行）
    std::vector<const char*> features;   // 含まれる機能（チップ）
    std::vector<const char*> includes;   // 同梱物（箇条書き）
    const char* cardImage;     // assets/editor/launcher/ の PNG 名（無い時はグラデ + アイコンへフォールバック）
    const char* iconGlyph;     // Lucide のグリフ（フォールバックの絵とチップに使う）
    uint32_t    accent;        // 0xRRGGBB: カードのグロー / フォールバックのグラデ
    uint32_t    gradA;         // フォールバックのグラデ（上）
    uint32_t    gradB;         // フォールバックのグラデ（下）
};

// ★新しいテンプレートを増やす手順:
//   1. src/project/ProjectTemplates.cpp の GetFiles() に ID とファイル表を足す（必ず assets/scenes/main.json を含める）
//   2. 下の表へ 1 行足す（画像が無ければ cardImage は "" でよい。グラデ + アイコンになる）
//   3. 画像は docs/LAUNCHER.md の手順でエンジン内レンダリング → assets/editor/launcher/ へ置く
// tests/launcher_logic_test.cpp が「ID の一意性 / GetFiles に実在 / 画像ファイルの実在」を検査する。
inline const std::vector<TemplateDef>& Templates()
{
    static const std::vector<TemplateDef> kT = {
        { "fps", "FPS", "一人称シューター",
          "物理ベースの射撃レンジ",
          "的と木箱が並ぶ射撃レンジ「STEEL RANGE」。マウス視点の移動・ジャンプ・射撃までがすぐ遊べます。"
          "タイトル → ゲーム → クリアの 3 シーン構成で、画面遷移も組んであります。",
          { "一人称視点", "CharacterController 物理", "レイキャスト射撃", "HUD / UI アニメ", "ポストプロセス", "SSAO" },
          { "シーン 3 つ（タイトル / ゲーム / クリア。遷移つき）",
            "一人称プレイヤー操作（Lua）",
            "的とゲーム進行（Lua）" },
          "tmpl_fps.png", ICON_CROSSHAIR, 0x2F8CFF, 0x0B1626, 0x111C33 },
        { "tps", "TPS", "三人称アクション",
          "コイン集めのアクション",
          "コインを集めてゴールを目指す「COIN RUSH」。追従カメラ・物理ジャンプ・ゴール判定が入った、"
          "三人称アクションの最小構成です。",
          { "三人称追従カメラ", "物理ジャンプ", "コイン収集", "ゴールトリガー", "画面遷移", "UI アニメ" },
          { "シーン 3 つ（タイトル / ゲーム / クリア。遷移つき）",
            "三人称プレイヤー操作と追従カメラ（Lua）",
            "コインとゲーム進行（Lua）" },
          "tmpl_tps.png", ICON_T_CHARACTER, 0x3FB6A8, 0x0A1B1F, 0x0F2A2D },
        { "2d", "2D", "横スクロール",
          "プラットフォーマー 1 コース",
          "動く床・トゲ・コイン・ゴール旗を並べた横スクロールの 1 コース「SKY HOPPER」。"
          "コヨーテタイムと先行入力つきのジャンプが入っています。",
          { "横スクロール", "2D 当たり判定", "動く床", "トゲ / 落下判定", "コイン", "ゴール旗" },
          { "シーン 3 つ（タイトル / ゲーム / クリア。遷移つき）",
            "2D プレイヤー操作（Lua）",
            "コースとゲーム進行（Lua）" },
          "tmpl_2d.png", ICON_GAMEPAD, 0xE58A55, 0x1E1310, 0x2E1C14 },
        { "empty", "空", "最小構成",
          "グリッドとキューブだけの土台",
          "何も決まっていない状態から始める最小構成です。床のグリッド・キューブ・ライト・カメラと、"
          "Lua 部品のサンプル（Spinner.lua）だけが入っています。",
          { "グリッド床", "キューブ", "ライト", "カメラ", "Lua 部品サンプル" },
          { "シーン 1 つ（main）",
            "Lua 部品のサンプル（Spinner.lua）",
            "空のゲームスクリプト（game.lua）" },
          "tmpl_empty.png", ICON_T_GRID, 0x8E919C, 0x121317, 0x1A1C22 },
    };
    return kT;
}

inline const TemplateDef* FindTemplate(const std::string& id)
{
    for (const auto& t : Templates()) if (id == t.id) return &t;
    return nullptr;
}

}  // namespace dx12e::launcher
