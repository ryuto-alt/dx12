#include "scene/SceneFormatV2.h"

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <system_error>
#include <thread>
#include <vector>

// CMake が src/scene/scene_defaults_v2.json から生成する（kSceneDefaultsV2Bytes）。
// ファイルを実行時に探さない＝Game.exe（pak 配布）でも同じ表が使える。
#include "scene_defaults_v2_data.h"

namespace dx12e::scenefmt
{

using json = nlohmann::json;

double NormalizeDouble(double v)
{
    if (!std::isfinite(v) || std::fabs(v) > static_cast<double>(FLT_MAX)) return v;
    // 速い経路: 2^24 未満の整数値は float32 で正確で、最短表記も整数のまま＝値は変わらない（0.0 / 1.0 / 90.0 など大半）。
    // 負のゼロもここで素通し（符号を保つ）。
    if (std::fabs(v) < 16777216.0 && v == std::trunc(v)) return v;
    const float f = static_cast<float>(v);
    if (static_cast<double>(f) != v) return v;   // float32 で正確に表せない double は触らない

    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof(buf), f);   // float の最短往復表記
    if (r.ec != std::errc()) return v;
    double back = 0.0;
    const auto p = std::from_chars(buf, r.ptr, back);
    if (p.ec != std::errc()) return v;
    return back;
}

void NormalizeFloats(json& j)
{
    switch (j.type())
    {
    case json::value_t::number_float:
        j = NormalizeDouble(j.get<double>());
        break;
    case json::value_t::array:
    case json::value_t::object:
        for (auto& child : j) NormalizeFloats(child);
        break;
    default:
        break;
    }
}

const json& DefaultsV2()
{
    static const json table = []() {
        json t = json::parse(reinterpret_cast<const char*>(kSceneDefaultsV2Bytes),
                             reinterpret_cast<const char*>(kSceneDefaultsV2Bytes) + sizeof(kSceneDefaultsV2Bytes),
                             nullptr, /*allow_exceptions=*/false);
        if (t.is_discarded() || !t.is_object()) return json::object();
        NormalizeFloats(t);   // 比較は正規化後に行う（表も同じ正規化をした状態で持つ）
        return t;
    }();
    return table;
}

// 厳密な等価。nlohmann の == は -0.0 == 0.0 を真にするので、そのまま使うと負のゼロが省略されて 0.0 に戻り、
// float32 のビットが変わってしまう（実シーンの rotation に -0.0 が居る）。浮動小数はビットで比べ、
// 整数と浮動小数は別物（整数同士は符号付き / 符号なしの差を無視して値で比べる）。
static bool ExactEqual(const json& a, const json& b)
{
    if (a.is_number_float() || b.is_number_float())
    {
        if (!(a.is_number_float() && b.is_number_float())) return false;
        const double x = a.get<double>(), y = b.get<double>();
        return std::memcmp(&x, &y, sizeof(double)) == 0;
    }
    if (a.is_array() && b.is_array())
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!ExactEqual(a[i], b[i])) return false;
        return true;
    }
    if (a.is_object() && b.is_object())
    {
        if (a.size() != b.size()) return false;
        for (auto it = a.begin(); it != a.end(); ++it)
        {
            const auto jt = b.find(it.key());
            if (jt == b.end() || !ExactEqual(it.value(), *jt)) return false;
        }
        return true;
    }
    if (a.type() != b.type() && !(a.is_number_integer() && b.is_number_integer())) return false;
    return a == b;
}

void StripDefaults(json& ej, const json& table)
{
    if (!ej.is_object()) return;
    for (auto it = ej.begin(); it != ej.end(); ++it)
    {
        if (!it.value().is_object()) continue;
        const auto tit = table.find(it.key());
        if (tit == table.end() || !tit->is_object()) continue;
        json& comp = it.value();
        for (auto fit = tit->begin(); fit != tit->end(); ++fit)
        {
            const auto cit = comp.find(fit.key());
            if (cit != comp.end() && ExactEqual(*cit, fit.value()))
                comp.erase(cit);
        }
    }
}

void InflateDefaults(json& ej, const json& table)
{
    if (!ej.is_object()) return;
    for (auto it = ej.begin(); it != ej.end(); ++it)
    {
        if (!it.value().is_object()) continue;
        const auto tit = table.find(it.key());
        if (tit == table.end() || !tit->is_object()) continue;
        json& comp = it.value();
        for (auto fit = tit->begin(); fit != tit->end(); ++fit)
            if (!comp.contains(fit.key()))
                comp[fit.key()] = fit.value();
    }
}

bool IsV2(const json& root)
{
    if (!root.is_object()) return false;
    const auto it = root.find("version");
    return it != root.end() && it->is_number_integer() && it->get<long long>() >= kSceneVersionV2;
}

bool InflateScene(json& root)
{
    if (!IsV2(root)) return false;
    const auto eit = root.find("entities");
    if (eit == root.end() || !eit->is_array()) return true;
    const json& table = DefaultsV2();
    for (auto& ej : *eit) InflateDefaults(ej, table);
    return true;
}

// [begin, end) の分割を数スレッドで回す。エンティティは互いに独立（共有するのは読み取り専用の表だけ）なので安全。
// 小さいシーンはスレッドを起こさない。10 万体のシーンの保存でメインスレッドを塞ぐ時間を縮めるためのもの。
template <typename Fn>
static void ParallelRanges(size_t n, Fn&& fn, bool allowThreads = true)
{
    const unsigned hw = (std::max)(1u, std::thread::hardware_concurrency());
    const size_t workers = (n < 4096 || !allowThreads) ? 1 : (std::min)(static_cast<size_t>((std::max)(1u, hw / 2)), static_cast<size_t>(6));
    if (workers <= 1) { fn(static_cast<size_t>(0), n); return; }
    const size_t chunk = (n + workers - 1) / workers;
    // ★スレッドの中で例外が抜けると std::terminate でプロセスごと落ちる（dump は不正な UTF-8 で type_error を投げる）。
    //   捕まえて join の後で呼び出し元へ投げ直す（直列のときと同じく Save の呼び出し側が受け取れる）。
    std::vector<std::exception_ptr> errors(workers);
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (size_t w = 1; w < workers; ++w)
    {
        const size_t b = w * chunk, e = (std::min)(n, b + chunk);
        if (b >= e) break;
        threads.emplace_back([&fn, &errors, w, b, e]() {
            try { fn(b, e); } catch (...) { errors[w] = std::current_exception(); }
        });
    }
    try { fn(static_cast<size_t>(0), (std::min)(n, chunk)); } catch (...) { errors[0] = std::current_exception(); }
    for (auto& t : threads) t.join();
    for (auto& e : errors) if (e) std::rethrow_exception(e);
}

void ConvertToV2(json& root)
{
    if (!root.is_object()) return;
    // ルート設定は小さいので直列で正規化（entities は下で並列）。
    for (auto it = root.begin(); it != root.end(); ++it)
        if (it.key() != "entities") NormalizeFloats(it.value());
    root["version"] = kSceneVersionV2;
    const auto eit = root.find("entities");
    if (eit == root.end() || !eit->is_array()) return;
    const json& table = DefaultsV2();   // 初回の構築はここ（スレッドを起こす前）で済ませる
    json& ents = *eit;
    ParallelRanges(ents.size(), [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i)
        {
            json& ej = ents[i];
            if (!ej.is_object()) continue;
            NormalizeFloats(ej);
            // parentGuid があるときは index を書かない（index は挿入・削除で全体がずれ、差分が広がる）。
            if (ej.contains("parentGuid")) ej.erase("parent");
            StripDefaults(ej, table);
        }
    });
}

std::string DumpSceneV2(const json& root, bool allowThreads)
{
    const auto eit = root.is_object() ? root.find("entities") : root.end();
    if (!root.is_object() || eit == root.end() || !eit->is_array())
        return root.dump(2) + "\n";

    std::string out;
    out.reserve(256 + eit->size() * 160);
    out += "{\n";
    auto emitSetting = [&](const std::string& key, const json& value) {
        out += "  ";
        out += json(key).dump();
        out += ": ";
        std::string body = value.dump(2);
        // 字下げを 2 つ足す。文字列内の改行は \n にエスケープ済みなので生の '\n' は構造の改行だけ。
        for (char c : body)
        {
            out += c;
            if (c == '\n') out += "  ";
        }
        out += ",\n";
    };
    if (const auto vit = root.find("version"); vit != root.end()) emitSetting("version", *vit);
    for (auto it = root.begin(); it != root.end(); ++it)
    {
        if (it.key() == "version" || it.key() == "entities") continue;
        if (it.key() == "parts" && it.value().is_array())
        {
            // 分割保存の一覧は 1 要素 1 行（AI が開く前に中身の当たりを付けるための目次）。
            out += "  \"parts\": [";
            for (size_t i = 0; i < it.value().size(); ++i)
            {
                out += i ? ",\n    " : "\n    ";
                out += it.value()[i].dump();
            }
            out += it.value().empty() ? "],\n" : "\n  ],\n";
            continue;
        }
        emitSetting(it.key(), it.value());
    }
    if (eit->empty())
    {
        out += "  \"entities\": []\n}\n";
        return out;
    }
    // 1 要素 1 行・区切りなし（nlohmann の compact dump は , と : の後に空白を入れない）。
    // 各エンティティの文字列化は独立なので並列に作ってから順に連結する。
    std::vector<std::string> lines(eit->size());
    ParallelRanges(eit->size(), [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) lines[i] = (*eit)[i].dump();
    }, allowThreads);
    out += "  \"entities\": [\n";
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i) out += ",\n";
        out += lines[i];
    }
    out += "\n  ]\n}\n";
    return out;
}

} // namespace dx12e::scenefmt
