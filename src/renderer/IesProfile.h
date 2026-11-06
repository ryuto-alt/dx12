#pragma once
// IesProfile.h - IES LM-63 (.ies) の配光ファイルを読み、縦角 → 相対光度の 1D プロファイルへ焼く。
//
// ヘッダオンリー・標準ライブラリのみ（GPU 非依存、tests/ies_profile_test.cpp で検証）。
//
// 方針（UE の IES プロファイルに合わせた最小構成）:
//   - 縦角は「光軸からの角度 θ（0..180°）」。測光タイプ C（縦角 0 = 真下）はそのまま、
//     タイプ A / B（縦角 0 = 軸、±に振れる）は |角度| に折り返して使う。
//   - 水平角は平均を取って 1D にする（軸対称の配光 = 懐中電灯・ダウンライトは完全に保存される。
//     非対称の配光は水平方向の差が失われる）。
//   - 結果は最大値で 1.0 に正規化する。実際の絶対光度（cd）はライトの intensity が与える
//     （UE の「IES を明るさの形だけに使う」と同じ。物理ライティング単位でも intensity[cd] の倍率になる）。
//   - データの範囲外（例: 0..90° しか無いダウンライトの 90° より外）は 0。
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dx12e
{

struct IesProfile
{
    bool                ok = false;
    std::string         error;
    int                 photometricType = 1;      // 1=C / 2=B / 3=A
    std::vector<float>  anglesDeg;                // 光軸からの角度（昇順、0 始まり）
    std::vector<float>  candela;                  // 水平方向に平均した光度（anglesDeg と同数、未正規化）
    float               maxCandela = 0.0f;

    // 光軸からの角度 θ（度）での相対光度（max = 1）。範囲外 0。
    float Eval(float thetaDeg) const
    {
        if (!ok || anglesDeg.empty() || maxCandela <= 0.0f) return 1.0f;
        if (thetaDeg < anglesDeg.front()) return candela.front() / maxCandela;   // 先頭より内側は先頭値
        if (thetaDeg > anglesDeg.back()) return 0.0f;
        size_t hi = 1;
        while (hi < anglesDeg.size() && anglesDeg[hi] < thetaDeg) ++hi;
        if (hi >= anglesDeg.size()) return candela.back() / maxCandela;
        const float a0 = anglesDeg[hi - 1], a1 = anglesDeg[hi];
        const float t  = (a1 > a0) ? (thetaDeg - a0) / (a1 - a0) : 0.0f;
        return ((1.0f - t) * candela[hi - 1] + t * candela[hi]) / maxCandela;
    }

    // θ = 0..180° を n 点に等分して焼く（out[i] は θ = 180 * i / (n-1)）。0..1。
    void Bake(float* out, size_t n) const
    {
        for (size_t i = 0; i < n; ++i)
        {
            const float th = (n > 1) ? 180.0f * static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
            out[i] = Eval(th);
        }
    }
};

namespace ies_detail
{
inline bool IsSep(char c) { return std::isspace(static_cast<unsigned char>(c)) || c == ','; }

// 先頭から次のトークンを取る。終端で false。
inline bool NextToken(std::string_view& s, std::string_view& tok)
{
    size_t i = 0;
    while (i < s.size() && IsSep(s[i])) ++i;
    if (i >= s.size()) { s = {}; return false; }
    size_t j = i;
    while (j < s.size() && !IsSep(s[j])) ++j;
    tok = s.substr(i, j - i);
    s.remove_prefix(j);
    return true;
}
inline bool NextNumber(std::string_view& s, double& v)
{
    std::string_view t;
    if (!NextToken(s, t)) return false;
    const std::string tmp(t);
    char* end = nullptr;
    v = std::strtod(tmp.c_str(), &end);
    return end && end != tmp.c_str();
}
} // namespace ies_detail

// LM-63 テキストを解析する。失敗時は ok=false + error。
inline IesProfile ParseIes(std::string_view text)
{
    using namespace ies_detail;
    IesProfile p;

    // "TILT=" の行を探す（それ以前は [KEYWORD] 行）。
    size_t tiltPos = text.find("TILT=");
    if (tiltPos == std::string_view::npos) { p.error = "TILT= 行がありません"; return p; }
    size_t eol = text.find('\n', tiltPos);
    std::string_view tiltLine = text.substr(tiltPos, (eol == std::string_view::npos ? text.size() : eol) - tiltPos);
    std::string_view rest = (eol == std::string_view::npos) ? std::string_view{} : text.substr(eol + 1);

    double v = 0.0;
    if (tiltLine.find("INCLUDE") != std::string_view::npos)
    {
        // lamp-to-luminaire geometry, N, angles[N], factors[N] を読み飛ばす
        double geom = 0.0, n = 0.0;
        if (!NextNumber(rest, geom) || !NextNumber(rest, n) || n < 0.0 || n > 1000.0) { p.error = "TILT=INCLUDE のデータが壊れています"; return p; }
        for (int i = 0; i < static_cast<int>(n) * 2; ++i)
            if (!NextNumber(rest, v)) { p.error = "TILT=INCLUDE のデータが足りません"; return p; }
    }

    double numLamps, lumens, multiplier, nV, nH, phType, units, w, l, h;
    if (!NextNumber(rest, numLamps) || !NextNumber(rest, lumens) || !NextNumber(rest, multiplier)
        || !NextNumber(rest, nV) || !NextNumber(rest, nH) || !NextNumber(rest, phType)
        || !NextNumber(rest, units) || !NextNumber(rest, w) || !NextNumber(rest, l) || !NextNumber(rest, h))
    { p.error = "測光ヘッダが読めません"; return p; }
    double ballast, future, watts;
    if (!NextNumber(rest, ballast) || !NextNumber(rest, future) || !NextNumber(rest, watts))
    { p.error = "バラスト行が読めません"; return p; }

    const int nv = static_cast<int>(nV), nh = static_cast<int>(nH);
    if (nv < 1 || nv > 4096 || nh < 1 || nh > 4096) { p.error = "角度数が範囲外です"; return p; }
    p.photometricType = static_cast<int>(phType);

    std::vector<double> va(static_cast<size_t>(nv)), ha(static_cast<size_t>(nh));
    for (auto& x : va) if (!NextNumber(rest, x)) { p.error = "縦角が足りません"; return p; }
    for (auto& x : ha) if (!NextNumber(rest, x)) { p.error = "水平角が足りません"; return p; }
    std::vector<double> cd(static_cast<size_t>(nv) * static_cast<size_t>(nh));
    for (int j = 0; j < nh; ++j)
        for (int i = 0; i < nv; ++i)
            if (!NextNumber(rest, cd[static_cast<size_t>(j) * static_cast<size_t>(nv) + static_cast<size_t>(i)]))
            { p.error = "光度データが足りません"; return p; }

    // 水平方向に平均
    std::vector<double> avg(static_cast<size_t>(nv), 0.0);
    for (int i = 0; i < nv; ++i)
    {
        double s = 0.0;
        for (int j = 0; j < nh; ++j) s += cd[static_cast<size_t>(j) * static_cast<size_t>(nv) + static_cast<size_t>(i)];
        avg[static_cast<size_t>(i)] = s / static_cast<double>(nh) * (multiplier > 0.0 ? multiplier : 1.0);
    }

    // 光軸からの角度に直す。タイプ C は縦角そのまま。A / B は |角度| に折り返す（軸 = 0）。
    std::vector<std::pair<float, float>> pts;
    pts.reserve(static_cast<size_t>(nv));
    for (int i = 0; i < nv; ++i)
    {
        double a = va[static_cast<size_t>(i)];
        if (p.photometricType != 1) a = std::fabs(a);
        pts.emplace_back(static_cast<float>(a), static_cast<float>(avg[static_cast<size_t>(i)]));
    }
    std::sort(pts.begin(), pts.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    // 同じ角度が重なった（A/B の ±）ときは平均する
    for (size_t i = 0; i < pts.size();)
    {
        size_t j = i; float s = 0.0f;
        while (j < pts.size() && pts[j].first == pts[i].first) { s += pts[j].second; ++j; }
        p.anglesDeg.push_back(pts[i].first);
        p.candela.push_back(s / static_cast<float>(j - i));
        i = j;
    }
    for (float c : p.candela) p.maxCandela = std::max(p.maxCandela, c);
    if (p.maxCandela <= 0.0f) { p.error = "光度がすべて 0 です"; return p; }
    p.ok = true;
    return p;
}

} // namespace dx12e
