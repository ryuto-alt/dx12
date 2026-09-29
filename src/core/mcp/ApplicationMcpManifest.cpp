// ===========================================================================
// MCP: マニフェスト（method ごとのメタ情報）と引数の中央検査
// ---------------------------------------------------------------------------
// 設計は docs/MCP_ENHANCEMENT_DESIGN.md §4.2.2（マニフェスト）/ §4.3（エラー）。
//
//   McpDefine(names, McpMeta, fn)  … 新規 method はこちら。params を中央検査する（McpValidateMeta）。
//   ApplicationMcpManifestData.inc … 既存 174 method の meta を持つデータ表（生成物。一度きりの
//                                   ブートストラップで、以後は直接編集してよい）。
//   ApplyMcpManifest()             … 起動後の最初の MCP コマンドで表を組んだ直後に、表の内容を
//                                   m_mcpMethods へ流し込む。表に無い method は paramSpec から自動導出
//                                   （derived。ctest McpManifestTests は 0 件を要求する）。
//   describe_mcp_manifest          … 全 method の meta を JSON で返す。TS サーバはこれを引いて
//                                   エンジンを再ビルドしても TS を再起動せずに新 method を呼ぶ。
//
// ★既存 174 method の名前・引数・成功時の応答形・error 文字列は 1 つも変えない。
//   データ表由来の引数は enforce=false（中央検査の対象外）＝既存クライアントを壊さない。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpManifestBuild.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <unordered_map>

namespace dx12e
{
using namespace appdetail;

namespace appdetail
{
// プロセス起動時刻（epoch ms）。この TU の静的初期化で 1 度だけ取る（ping.engineStartedAtMs）。
static const long long kProcessStartedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
long long McpEngineStartedAtMs() { return kProcessStartedAtMs; }

// ---------------------------------------------------------------------------
// 中央検査
// ---------------------------------------------------------------------------
namespace
{
const char* JsonTypeWord(const nlohmann::json& v)
{
    if (v.is_boolean()) return "bool";
    if (v.is_number_integer()) return "int";
    if (v.is_number()) return "number";
    if (v.is_string()) return "string";
    if (v.is_array()) return "array";
    if (v.is_object()) return "object";
    return "null";
}

bool IsIntegral(const nlohmann::json& v)
{
    if (v.is_number_integer()) return true;
    if (v.is_number_float()) { const double d = v.get<double>(); return d == static_cast<double>(static_cast<long long>(d)); }
    return false;
}

bool AllNumbers(const nlohmann::json& v, size_t n)
{
    if (!v.is_array() || v.size() != n) return false;
    return std::all_of(v.begin(), v.end(), [](const nlohmann::json& x) { return x.is_number(); });
}

// 型が合っていれば true。合わないときは期待する型の説明を expected へ入れる。
bool TypeMatches(const nlohmann::json& v, const McpParam& p, std::string& expected)
{
    const std::string& t = p.type;
    expected = t;
    if (t == "bool")   return v.is_boolean();
    if (t == "int")    return IsIntegral(v);
    if (t == "number") return v.is_number();
    if (t == "string" || t == "assetPath" || t == "enum") return v.is_string();
    if (t == "vec2")   { expected = "vec2 ([x,y])";     return AllNumbers(v, 2); }
    if (t == "vec3")   { expected = "vec3 ([x,y,z])";   return AllNumbers(v, 3); }
    if (t == "vec4")   { expected = "vec4 ([x,y,z,w])"; return AllNumbers(v, 4); }
    if (t == "entityRef") { expected = "entityRef (int id or string name)"; return IsIntegral(v) || v.is_string(); }
    if (t == "object") return v.is_object();
    if (t == "array")  return v.is_array();
    return true;   // any
}
} // namespace

void McpValidateMeta(const McpMeta& meta, const nlohmann::json& params, const std::string& method)
{
    using json = nlohmann::json;
    for (const McpParam& p : meta.params)
    {
        if (!p.enforce) continue;
        const auto it = params.find(p.name);
        const bool present = it != params.end() && !it->is_null();
        if (!present)
        {
            if (!p.required) continue;
            McpError err(McpErr::InvalidParam, method + ": missing required param '" + p.name + "'",
                         "'" + p.name + "' (" + p.type + ") を渡す" + (p.desc.empty() ? std::string() : "。" + p.desc));
            err.name  = "E_MISSING_PARAM";
            err.cause = "必須の引数 '" + p.name + "' が渡されていない";
            err.details = {{"param", p.name}, {"type", p.type}};
            throw err;
        }

        std::string expected;
        if (!TypeMatches(*it, p, expected))
        {
            McpError err(McpErr::InvalidParam,
                         method + ": '" + p.name + "' must be " + expected + " (got " + JsonTypeWord(*it) + ")",
                         "'" + p.name + "' は " + expected + " で渡す" + (p.desc.empty() ? std::string() : "。" + p.desc));
            err.name  = "E_BAD_TYPE";
            err.cause = "引数 '" + p.name + "' の型が違う";
            err.details = {{"param", p.name}, {"expected", expected}, {"got", JsonTypeWord(*it)}};
            throw err;
        }

        if (p.type == "enum" && !p.enumValues.empty())
        {
            const std::string got = it->get<std::string>();
            if (std::find(p.enumValues.begin(), p.enumValues.end(), got) == p.enumValues.end())
            {
                McpError err(McpErr::InvalidParam,
                             method + ": '" + p.name + "' has no value '" + got + "'",
                             "'" + p.name + "' は有効値のどれかで渡す（error_values を参照）",
                             p.enumValues);
                err.name       = "E_BAD_ENUM";
                err.cause      = "'" + p.name + "' は列挙値。渡された '" + got + "' は候補に無い";
                err.didYouMean = McpSuggest(got, p.enumValues, 3);
                err.details    = {{"param", p.name}, {"got", got}};
                throw err;
            }
        }

        if ((p.type == "int" || p.type == "number") && it->is_number())
        {
            const double v = it->get<double>();
            if ((p.hasMin && v < p.min) || (p.hasMax && v > p.max))
            {
                json range = json::object();
                if (p.hasMin) range["min"] = p.min;
                if (p.hasMax) range["max"] = p.max;
                McpError err(McpErr::InvalidParam,
                             method + ": '" + p.name + "' is out of range (" + McpNumberText(v) + ")",
                             "'" + p.name + "' は範囲内の値で渡す（error_details の min / max を参照）");
                err.name  = "E_OUT_OF_RANGE";
                err.cause = "引数 '" + p.name + "' が許される範囲を出ている";
                err.details = {{"param", p.name}, {"value", v}, {"range", range}};
                throw err;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// データ表（既存 method の meta）
// ---------------------------------------------------------------------------
namespace
{
using namespace mcpdata;   // ManifestRows / P / M（core/mcp/McpManifestBuild.h）

// ★生成物。tools/mcp-server/scripts/gen_engine_manifest.mjs が一度きりのブートストラップとして吐いた。
//   以後は直接編集してよい（再生成すると手編集が消える）。
//   ここでは ManifestRows へ全 method の行を積む FillGeneratedManifestRows(rows) が定義される。
#include "core/mcp/ApplicationMcpManifestData.inc"

// ---------------------------------------------------------------------------
// 表に無い method の自動導出（paramSpec から。型だけ・category は "uncategorized"）
// ---------------------------------------------------------------------------
McpMeta DeriveMeta(const std::string& name, const char* paramSpec)
{
    McpMeta m;
    m.summary  = name;
    m.category = "uncategorized";
    m.effect   = (name.rfind("get_", 0) == 0 || name.rfind("list_", 0) == 0 || name.rfind("describe_", 0) == 0)
                 ? McpEffect::Read : McpEffect::WriteScene;
    m.params   = McpParseParamSpec(paramSpec ? paramSpec : "");
    return m;
}

// ---------------------------------------------------------------------------
// JSON 化
// ---------------------------------------------------------------------------
nlohmann::json ParamToJson(const McpParam& p)
{
    using json = nlohmann::json;
    json j = {{"name", p.name}, {"type", p.type}, {"required", p.required}};
    if (!p.enumValues.empty()) j["enum"] = p.enumValues;
    if (p.hasMin) j["min"] = p.min;
    if (p.hasMax) j["max"] = p.max;
    if (p.hasDefault)
    {
        json d = json::parse(p.defaultJson, nullptr, /*allow_exceptions=*/false);
        j["default"] = d.is_discarded() ? json(p.defaultJson) : d;
    }
    if (!p.desc.empty()) j["desc"] = p.desc;
    if (p.enforce) j["enforce"] = true;
    return j;
}

nlohmann::json MetaToJson(const std::string& name, const McpMeta& m, bool derived, bool brief)
{
    using json = nlohmann::json;
    json j = {{"name", name},
              {"category", m.category},
              {"summary", m.summary},
              {"keywords", m.keywords},
              {"effect", McpEffectName(m.effect)},
              {"mode", m.mode},
              {"timeoutMs", m.timeoutMs},
              {"idempotent", m.idempotent},
              {"deferred", m.deferred},
              {"dryRun", m.dryRun}};
    if (!m.group.empty())  j["group"]  = m.group;
    if (!m.target.empty()) j["target"] = m.target;
    j["aliases"] = m.aliases;
    if (!m.expose.empty()) j["expose"] = m.expose;
    if (!brief)
    {
        json params = json::array();
        for (const McpParam& p : m.params) params.push_back(ParamToJson(p));
        j["params"] = std::move(params);
        json next = json::array();
        for (const McpNext& n : m.next) next.push_back({{"tool", n.tool}, {"when", n.when}});
        j["next"] = std::move(next);
        json ex = json::array();
        for (const McpExample& e : m.examples)
        {
            json a = json::parse(e.argsJson, nullptr, /*allow_exceptions=*/false);
            ex.push_back({{"args", a.is_discarded() ? json::object() : a}, {"note", e.note}});
        }
        j["examples"] = std::move(ex);
    }
    j["source"] = derived ? "derived" : "meta";
    return j;
}
} // namespace
} // namespace appdetail

// ---------------------------------------------------------------------------
// ApplyMcpManifest — 表を組んだ直後に 1 度だけ呼ぶ
// ---------------------------------------------------------------------------
void Application::ApplyMcpManifest()
{
    mcpdata::ManifestRows rows;
    rows.reserve(200);
    FillGeneratedManifestRows(rows);

    std::unordered_map<std::string, size_t> byName;
    for (size_t i = 0; i < rows.size(); ++i)
        if (!byName.emplace(rows[i].name, i).second)
            Logger::Error("MCP manifest: duplicate row '{}' in the data table", rows[i].name);

    int fromTable = 0, direct = 0, derived = 0;
    for (auto& [name, entry] : m_mcpMethods)
    {
        if (entry.hasMeta) { ++direct; continue; }   // McpDefine(names, McpMeta, fn) で直接渡された
        const auto it = byName.find(name);
        if (it != byName.end())
        {
            entry.meta    = rows[it->second].meta;
            entry.hasMeta = true;
            entry.derived = false;
            ++fromTable;
        }
        else
        {
            entry.meta    = DeriveMeta(name, entry.paramSpec);
            entry.hasMeta = true;   // meta は入れるが params は enforce=false なので中央検査は走らない
            entry.derived = true;
            ++derived;
        }
    }

    // manifestHash: 全 method（名前順）の meta をキー順固定の JSON にして FNV-1a 64。
    // unordered_map の走査順に依存しないよう、名前をソートしてから連結する。
    std::vector<const std::string*> names;
    names.reserve(m_mcpMethods.size());
    for (const auto& kv : m_mcpMethods) names.push_back(&kv.first);
    std::sort(names.begin(), names.end(), [](const std::string* a, const std::string* b) { return *a < *b; });
    uint64_t h = McpFnv1a64("mcp-manifest/1\n");
    for (const std::string* n : names)
    {
        const McpMethodEntry& e = m_mcpMethods.at(*n);
        h = McpFnv1a64(McpMetaCanonical(*n, e.meta, e.paramSpec ? e.paramSpec : ""), h);
        h = McpFnv1a64("\n", h);
    }
    m_mcpManifestHash = McpHex16(h);
    Logger::Info("MCP manifest: {} methods (table {} / direct {} / derived {}) hash={}",
                 m_mcpMethods.size(), fromTable, direct, derived, m_mcpManifestHash);
}

// ---------------------------------------------------------------------------
// 近い method 名（未知 method のエラーと describe_mcp_manifest が共有）
// ---------------------------------------------------------------------------
std::vector<std::string> Application::SuggestMcpMethodNames(const std::string& method) const
{
    // 候補: 全 method 名 + 別名（dx12_ を外した名前 → 持ち主の method）。並びは名前順で決定的にする。
    std::vector<std::string> names;
    names.reserve(m_mcpMethods.size());
    for (const auto& kv : m_mcpMethods) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    std::vector<std::string> cands;
    std::unordered_map<std::string, std::string> owner;
    auto add = [&](const std::string& cand, const std::string& ownerName)
    {
        if (owner.emplace(cand, ownerName).second) cands.push_back(cand);
    };
    for (const auto& n : names)
    {
        add(n, n);
        for (const auto& a : m_mcpMethods.at(n).meta.aliases)
            add(a.rfind("dx12_", 0) == 0 ? a.substr(5) : a, n);
    }
    // dx12_ 接頭辞つきで渡された場合は外した名前も同じ目で探す（TS のツール名を撃ってしまう間違い）
    std::vector<std::string> queries{method};
    if (method.rfind("dx12_", 0) == 0) queries.push_back(method.substr(5));

    std::vector<std::string> out;
    auto pushUnique = [&](const std::string& n)
    {
        if (n != method && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    };
    if (queries.size() > 1 && m_mcpMethods.count(queries[1])) pushUnique(queries[1]);
    for (const auto& q : queries)
        for (const auto& hit : McpSuggest(q, cands, 5))
            pushUnique(owner[hit]);
    if (out.size() > 5) out.resize(5);
    return out;
}

// ---------------------------------------------------------------------------
// describe_mcp_manifest — meta を直接渡す最初の method（source:"meta"・中央検査の実例）
// ---------------------------------------------------------------------------
void Application::RegisterMcpManifestMethods()
{
    using json = nlohmann::json;

    McpDefine("describe_mcp_manifest", McpMeta{
        /*summary*/    "全 method のカテゴリ・副作用・引数・タイムアウトなどのメタ情報（マニフェスト）を返す。"
                       "manifestHash が変われば method 表が変わった合図",
        /*keywords*/   "manifest マニフェスト method 一覧 メタ 引数 スキーマ describe capabilities",
        /*category*/   "meta",
        /*group*/      "",
        /*target*/     "",
        /*effect*/     McpEffect::Read,
        /*mode*/       "any",
        /*timeoutMs*/  8000,
        /*idempotent*/ true,
        /*deferred*/   false,
        /*dryRun*/     "none",
        /*aliases*/    {},
        /*params*/     {P("method", "string", false, nullptr, nullptr, nullptr, nullptr,
                          "1 件だけ返す method 名（省略で全件）"),
                        P("category", "string", false, nullptr, nullptr, nullptr, nullptr,
                          "このカテゴリの method だけ返す（例 render / entity）"),
                        P("brief", "bool", false, nullptr, nullptr, nullptr, "false",
                          "true で params / examples / next を省く軽量版")},
        /*next*/       {{"ping", "manifestHash の変化を軽く検知する"}},
        /*examples*/   {{"{\"brief\":true}", "全 method の名前と要約だけ"},
                        {"{\"method\":\"set_transform\"}", "1 件の引数まで詳しく"}},
    }, DX12E_MCP_HANDLER
        {
            const std::string only     = params.value("method", std::string());
            const std::string category = params.value("category", std::string());
            const bool        brief    = params.value("brief", false);

            std::vector<std::string> names;
            names.reserve(m_mcpMethods.size());
            for (const auto& kv : m_mcpMethods) names.push_back(kv.first);
            std::sort(names.begin(), names.end());

            if (!only.empty() && m_mcpMethods.find(only) == m_mcpMethods.end())
            {
                McpError err(McpErr::UnknownMethod, "unknown method: " + only,
                             "method を省くと全 method を返す。名前は dx12_ 接頭辞なし（例 set_transform）");
                err.name       = "E_UNKNOWN_TOOL";
                err.cause      = "この名前の method は無い";
                err.didYouMean = SuggestMcpMethodNames(only);
                err.fix.push_back(MakeMcpFix("describe_mcp_manifest", json{{"brief", true}},
                                             "全 method の名前・カテゴリ・要約を引いて正しい名前を探す"));
                throw err;
            }

            std::map<std::string, int> categoryCount;
            json methods = json::array();
            for (const std::string& n : names)
            {
                const McpMethodEntry& e = m_mcpMethods.at(n);
                ++categoryCount[e.meta.category];
                if (!only.empty() && n != only) continue;
                if (!category.empty() && e.meta.category != category) continue;
                methods.push_back(MetaToJson(n, e.meta, e.derived, brief));
            }
            json categories = json::array();
            for (const auto& [id, count] : categoryCount) categories.push_back({{"id", id}, {"count", count}});

            resp["ok"] = true;
            resp["result"] = {{"protocol", 1},
                              {"manifestHash", m_mcpManifestHash},
                              {"engineVersion", kEngineVersion},
                              {"count", methods.size()},
                              {"total", m_mcpMethods.size()},
                              {"categories", std::move(categories)},
                              {"methods", std::move(methods)}};
        });

    // -----------------------------------------------------------------------
    // 動的登録の実機確認用のダミー method（環境変数 DX12_MCP_DEV_PROBE=1 のときだけ登録される）。
    // 通常起動では存在しない。エンジンを「method が増えた版」として起動し直したときに、MCP サーバ(TS)が
    // 再起動なしで dx12_tool_search / dx12_call / (expose:"core" なので) tools/list へ反映するかを確かめる。
    // -----------------------------------------------------------------------
    {
        char*  probeEnv = nullptr;
        size_t probeLen = 0;
        const bool probe = _dupenv_s(&probeEnv, &probeLen, "DX12_MCP_DEV_PROBE") == 0 && probeEnv && probeEnv[0] == '1';
        if (probeEnv) free(probeEnv);
        if (probe)
        {
            McpMeta pm;
            pm.summary    = "動的登録の確認用ダミー(DX12_MCP_DEV_PROBE=1 のときだけ存在)。message をそのまま返す";
            pm.keywords   = "dev probe dummy 動的登録 ダミー 確認用 manifest 再起動なし";
            pm.category   = "meta";
            pm.effect     = McpEffect::Read;
            pm.timeoutMs  = 5000;
            pm.idempotent = true;
            pm.expose     = "core";
            pm.params     = {P("message", "string", false, nullptr, nullptr, nullptr, "\"hello\"", "そのまま返す文字列")};
            pm.examples   = {{"{\"message\":\"hi\"}", "疎通確認"}};
            McpDefine("dev_probe", pm, DX12E_MCP_HANDLER
                {
                    resp["ok"]     = true;
                    resp["result"] = {{"probe", true}, {"message", params.value("message", std::string("hello"))},
                                      {"methodCount", m_mcpMethods.size()}};
                });
        }
    }
}

} // namespace dx12e
