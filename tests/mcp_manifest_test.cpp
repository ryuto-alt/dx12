// MCP マニフェスト（method ごとのメタ情報）の整合性テスト。
//
// ★エンジンをリンクしない。次の 2 つだけで検査する（GPU も entt も nlohmann も要らない）。
//   1. ソースをテキストとして読む（ApplicationMcp*.cpp の McpDefine / ApplicationInternal.h の McpErr）
//   2. 依存ゼロのヘッダ McpMeta.h / McpManifestBuild.h と、データ表 ApplicationMcpManifestData.inc を
//      そのまま include して、実際の McpMeta の中身を検査する
//
// 見張るもの
//   (a) McpDefine の全 method 名（"a|b" を展開）= データ表の全名。欠け / 余りがゼロ（derived が 0 件）。
//       meta を直接渡す多重定義（McpDefine(names, McpMeta{...}, fn)）は表の外で数える。
//   (b) 全エントリが有効な effect / category / mode / dryRun / timeoutMs>0 / summary 非空 / 引数の型。
//       申告表（paramSpec）と表の引数名が食い違っていない。
//   (c) FNV-1a 64・paramSpec パーサ・canonical JSON・近い名前の提案の単体テスト。
//   (d) McpErr のコード値がユニークで、既存値が動いていない。
//
// 実行: ctest --output-on-failure -R McpManifest

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/mcp/McpManifestBuild.h"

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool cond, const std::string& label)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("  NG  %s\n", label.c_str());
}

using namespace dx12e;
using namespace dx12e::mcpdata;

// ---- データ表（生成物）をそのまま取り込む ----
// ManifestRows / P / M は McpManifestBuild.h が持つ。エンジン側（ApplicationMcpManifest.cpp）と同じ形。
#include "core/mcp/ApplicationMcpManifestData.inc"

std::string ReadFile(const std::string& path, bool& ok)
{
    std::ifstream ifs(path, std::ios::binary);
    ok = static_cast<bool>(ifs);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, sep))
        if (!cur.empty()) out.push_back(cur);
    return out;
}

// pos から連続する C++ 文字列リテラルを読んでつなぐ。先頭が '"' でなければ false。
bool ReadLiteral(const std::string& s, size_t& pos, std::string& out)
{
    out.clear();
    bool any = false;
    for (;;)
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\t')) ++pos;
        if (pos >= s.size() || s[pos] != '"') return any;
        ++pos;
        while (pos < s.size() && s[pos] != '"')
        {
            if (s[pos] == '\\' && pos + 1 < s.size()) { out += s[pos + 1]; pos += 2; continue; }
            out += s[pos++];
        }
        if (pos < s.size()) ++pos;
        any = true;
    }
}

struct SourceDefine
{
    std::vector<std::string> names;   // "a|b" を展開
    std::string              spec;    // 文字列リテラルの申告表（無ければ空）
    bool                     literalSpec = false;
    bool                     direct = false;   // 第 2 引数が McpMeta（meta を直接渡す多重定義）
};

// ソース全体から McpDefine("...", ...) の呼び出しを拾う（インデントは問わない。行コメント内は除く）。
std::vector<SourceDefine> ScanDefines(const std::string& src)
{
    std::vector<SourceDefine> out;
    const std::string mark = "McpDefine(\"";
    size_t at = 0;
    while ((at = src.find(mark, at)) != std::string::npos)
    {
        const size_t lineStart = src.rfind('\n', at) == std::string::npos ? 0 : src.rfind('\n', at) + 1;
        const bool inComment = src.substr(lineStart, at - lineStart).find("//") != std::string::npos;
        const bool identBefore = at > 0 && (std::isalnum(static_cast<unsigned char>(src[at - 1])) || src[at - 1] == '_');
        size_t p = at + mark.size() - 1;
        at += mark.size();
        if (inComment || identBefore) continue;
        std::string names;
        if (!ReadLiteral(src, p, names)) continue;
        while (p < src.size() && (src[p] == ' ' || src[p] == ',' || src[p] == '\n' || src[p] == '\r' || src[p] == '\t')) ++p;
        SourceDefine d;
        d.names = Split(names, '|');
        if (p < src.size() && src[p] == '"') d.literalSpec = ReadLiteral(src, p, d.spec);
        else d.direct = src.compare(p, 7, "McpMeta") == 0;
        out.push_back(std::move(d));
    }
    return out;
}
} // namespace

int main()
{
#if !defined(DX12E_MCP_SOURCES) || !defined(DX12E_INTERNAL_H)
    std::printf("SKIP: ソースのパスが渡されていない\n");
    return 0;
#else
    std::printf("MCP マニフェスト: データ表 = McpDefine の全 method / meta の妥当性\n");

    // ================= (c) 依存ゼロ部品の単体テスト =================
    // FNV-1a 64 の既知ベクタ（公式のテストベクタ）
    Check(McpFnv1a64("") == 0xcbf29ce484222325ull, "FNV-1a 64 of empty string");
    Check(McpFnv1a64("a") == 0xaf63dc4c8601ec8cull, "FNV-1a 64 of \"a\"");
    Check(McpFnv1a64("foobar") == 0x85944171f73967e8ull, "FNV-1a 64 of \"foobar\"");
    // 継ぎ足し: hash(a+b) == hash(b, seed = hash(a))
    Check(McpFnv1a64("bar", McpFnv1a64("foo")) == McpFnv1a64("foobar"), "FNV-1a 64 chaining");
    Check(McpHex16(0x1ull) == "0000000000000001", "McpHex16 pads to 16 digits");
    Check(McpHex16(0xcbf29ce484222325ull) == "cbf29ce484222325", "McpHex16 of the FNV offset");

    // paramSpec パーサ
    {
        const auto ps = McpParseParamSpec("entity:int, name:string,skybox.envMapPath:any,flag,pos:vec3,bad:zzz,,name:bool");
        Check(ps.size() == 6, "McpParseParamSpec keeps 6 unique keys (got " + std::to_string(ps.size()) + ")");
        if (ps.size() == 6)
        {
            Check(ps[0].name == "entity" && ps[0].type == "int", "spec: entity:int");
            Check(ps[1].name == "name" && ps[1].type == "string", "spec: name:string (whitespace trimmed)");
            Check(ps[2].name == "skybox.envMapPath" && ps[2].type == "any", "spec: dotted key stays");
            Check(ps[3].name == "flag" && ps[3].type == "any", "spec: no type -> any");
            Check(ps[4].name == "pos" && ps[4].type == "vec3", "spec: vec3");
            Check(ps[5].name == "bad" && ps[5].type == "any", "spec: unknown type -> any");
        }
        Check(McpParseParamSpec("").empty(), "empty spec -> no params");
        Check(McpParamsToSpec(McpParseParamSpec("a:int,b:vec3,c:any")) == "a:int,b:vec3,c:any", "spec round trip");
    }

    // effect ⇄ 文字列
    {
        const McpEffect all[] = {McpEffect::Read, McpEffect::WriteScene, McpEffect::WriteSetting,
                                 McpEffect::WriteFile, McpEffect::Runtime, McpEffect::Guarded};
        std::set<std::string> seen;
        for (McpEffect e : all)
        {
            McpEffect back = McpEffect::Read;
            const std::string name = McpEffectName(e);
            Check(McpEffectFromName(name, back) && back == e, "effect round trip: " + name);
            seen.insert(name);
        }
        Check(seen.size() == 6, "6 distinct effect names");
        McpEffect dummy;
        Check(!McpEffectFromName("nope", dummy), "unknown effect name is rejected");
    }

    // canonical JSON とハッシュ: 同じ入力は同じ、変えると変わる
    {
        McpMeta a;
        a.summary = "s";
        a.category = "c";
        a.params = McpParseParamSpec("x:int");
        const std::string c1 = McpMetaCanonical("m", a, "x:int");
        const std::string c2 = McpMetaCanonical("m", a, "x:int");
        Check(c1 == c2, "canonical JSON is deterministic");
        Check(c1.find("\"name\":\"m\"") == 1, "canonical JSON starts with the name key");
        McpMeta b = a;
        b.timeoutMs += 1;
        Check(McpFnv1a64(McpMetaCanonical("m", b, "x:int")) != McpFnv1a64(c1), "hash changes when timeoutMs changes");
        McpMeta c = a;
        c.params[0].enforce = true;
        Check(McpFnv1a64(McpMetaCanonical("m", c, "x:int")) != McpFnv1a64(c1), "hash changes when a param flag changes");
        Check(McpFnv1a64(McpMetaCanonical("m", a, "x:number")) != McpFnv1a64(c1), "hash changes when paramSpec changes");
        Check(McpFnv1a64(McpMetaCanonical("n", a, "x:int")) != McpFnv1a64(c1), "hash changes when the name changes");
        McpMeta d = a;
        d.summary = "quote\" and \\ and \n newline 日本語";
        const std::string cd = McpMetaCanonical("m", d, "");
        Check(cd.find("quote\\\" and \\\\ and \\n newline 日本語") != std::string::npos, "canonical JSON escapes strings");
        // expose（Core 昇格の印）: 空なら canonical に出ない（既存 method のハッシュを変えない）／付けるとハッシュが変わる
        Check(c1.find("expose") == std::string::npos, "canonical JSON omits expose when empty (existing hashes stay the same)");
        McpMeta ex = a;
        ex.expose = "core";
        const std::string cx = McpMetaCanonical("m", ex, "x:int");
        Check(cx.find("\"expose\":\"core\"") != std::string::npos && cx.back() == '}', "canonical JSON carries expose:core at the end");
        Check(McpFnv1a64(cx) != McpFnv1a64(c1), "hash changes when expose is set");
        Check(McpExposeValid("") && McpExposeValid("core") && !McpExposeValid("all") && !McpExposeValid("Core"), "expose vocabulary");
        // M5: journal は false のとき canonical に出ない（既存 method のハッシュ不変）／true のとき末尾に出てハッシュが変わる
        Check(c1.find("journal") == std::string::npos, "canonical JSON omits journal when false (existing hashes stay the same)");
        McpMeta jm = a;
        jm.journal = true;
        const std::string cj = McpMetaCanonical("m", jm, "x:int");
        Check(cj.find(",\"journal\":true}") != std::string::npos && cj.back() == '}', "canonical JSON carries journal:true at the end");
        Check(McpFnv1a64(cj) != McpFnv1a64(c1), "hash changes when journal is set");
        // expose と journal の両方: expose が先（フィールドは足した順に末尾へ）
        jm.expose = "core";
        const std::string cej = McpMetaCanonical("m", jm, "x:int");
        Check(cej.find("\"expose\":\"core\",\"journal\":true}") != std::string::npos, "canonical JSON order: expose then journal");
        // dryRun の語彙: preview を追加した（none / native は不変）
        Check(McpDryRunValid("none") && McpDryRunValid("native") && McpDryRunValid("preview") && !McpDryRunValid("static") && !McpDryRunValid(""), "dryRun vocabulary includes preview");
        McpMeta pm = a;
        pm.dryRun = "preview";
        Check(McpFnv1a64(McpMetaCanonical("m", pm, "x:int")) != McpFnv1a64(c1), "hash changes when dryRun becomes preview");
        // McpApplySafetyFlags: 名前一覧だけが印を受ける。Read の method は preview にならない
        McpMeta sf; sf.effect = McpEffect::WriteFile;
        McpApplySafetyFlags("save_scene", sf);
        Check(sf.dryRun == "preview" && sf.journal, "safety flags: save_scene -> preview + journal");
        McpMeta so; so.effect = McpEffect::WriteScene;
        McpApplySafetyFlags("set_ssao", so);
        Check(so.dryRun == "none" && !so.journal, "safety flags: 名前が一覧に無い method は変わらない");
        McpMeta sr; sr.effect = McpEffect::Read;
        McpApplySafetyFlags("set_transform", sr);
        Check(sr.dryRun == "none", "safety flags: Read の method は preview にならない");
    }

    // 近い名前の提案
    {
        const std::vector<std::string> names = {"Player", "player_2", "Enemy_01", "Enemy_02", "Enemy_10", "Camera", "Sun", "プレイヤー", "プレイヤ2"};
        auto s = McpSuggest("player", names);
        Check(!s.empty() && s[0] == "Player", "suggest: case-only difference comes first");
        s = McpSuggest("Plyer", names);
        Check(!s.empty() && s[0] == "Player", "suggest: one-letter typo -> Player");
        s = McpSuggest("Enemy", names);
        Check(s.size() == 3 && s[0] == "Enemy_01", "suggest: prefix match lists the Enemy_* group");
        s = McpSuggest("Camra", names);
        Check(!s.empty() && s[0] == "Camera", "suggest: Camra -> Camera");
        s = McpSuggest("プレイヤ", names);
        Check(std::find(s.begin(), s.end(), "プレイヤー") != s.end(), "suggest: Japanese names are compared per character");
        s = McpSuggest("zzzzzzzz", names);
        Check(s.empty(), "suggest: nothing near -> empty");
        s = McpSuggest("Sun", names);
        Check(s.empty() || s[0] != "Sun", "suggest: never returns the exact match itself");
        std::vector<std::string> many;
        for (int i = 0; i < 20; ++i) many.push_back("Tree_" + std::to_string(i));
        Check(McpSuggest("Tree", many).size() == 5, "suggest: capped at 5");
        Check(McpSuggest("Tree", many, 2).size() == 2, "suggest: maxN respected");
        Check(McpEditDistance(std::string("kitten"), std::string("sitting")) == 3, "edit distance kitten/sitting = 3");
        Check(McpEditDistance(std::string("ab"), std::string("ba")) == 1, "edit distance counts a transposition as 1");
        // 1 万件でも走査が終わる（性能の下限確認ではなく、素朴実装が O(N) で回ることの確認）
        std::vector<std::string> big;
        for (int i = 0; i < 10000; ++i) big.push_back("Entity_" + std::to_string(i));
        Check(!McpSuggest("entity_5000", big).empty(), "suggest: 10k names");
        Check(McpEncodeUtf8(McpDecodeUtf8("aあ😀")) == "aあ😀", "UTF-8 decode/encode round trip");
    }

    // 型 / モード / dryRun の語彙
    Check(McpParamTypeValid("vec4") && McpParamTypeValid("entityRef") && McpParamTypeValid("assetPath") &&
          McpParamTypeValid("enum") && !McpParamTypeValid("float"), "param type vocabulary");
    Check(McpModeValid("any") && McpModeValid("editor") && McpModeValid("playing") && !McpModeValid("play"), "mode vocabulary");

    // ================= ソースの読み込み =================
    std::string allSrc;
    std::map<std::string, std::string> fileText;
    for (const std::string& path : Split(DX12E_MCP_SOURCES, '|'))
    {
        bool ok = false;
        const std::string t = ReadFile(path, ok);
        if (!ok) { std::printf("SKIP: %s を開けない（配布ツリー等）\n", path.c_str()); return 0; }
        fileText[path] = t;
        allSrc += t + "\n";
    }

    // ================= (a) McpDefine の名前 = データ表の名前 =================
    std::set<std::string> sourceNames, directNames;
    std::map<std::string, SourceDefine> defineOf;
    for (const auto& kv : fileText)
    {
        const bool isManifestTu = kv.first.find("ApplicationMcpManifest.cpp") != std::string::npos;
        for (const SourceDefine& d : ScanDefines(kv.second))
        {
            for (const std::string& n : d.names)
            {
                // ApplicationMcpManifest.cpp の McpDefine は meta を直接渡す多重定義（表の外）
                if (d.direct || isManifestTu) { directNames.insert(n); continue; }
                if (!sourceNames.insert(n).second)
                    Check(false, "McpDefine の名前が重複している: " + n);
                defineOf[n] = d;
            }
        }
    }

    ManifestRows rows;
    FillGeneratedManifestRows(rows);
    std::set<std::string> tableNames;
    std::map<std::string, const McpMeta*> metaOf;
    for (const ManifestRow& r : rows)
    {
        Check(tableNames.insert(r.name).second, std::string("データ表の行が重複している: ") + r.name);
        metaOf[r.name] = &r.meta;
    }

    std::string missing, extra;
    for (const std::string& n : sourceNames) if (!tableNames.count(n)) missing += (missing.empty() ? "" : ", ") + n;
    for (const std::string& n : tableNames)  if (!sourceNames.count(n)) extra   += (extra.empty() ? "" : ", ") + n;
    Check(missing.empty(), "McpDefine にあるのにデータ表に無い（derived になる）: " + missing +
          "  → ApplicationMcpManifestData.inc へ行を足すか、meta を直接渡す多重定義で書くこと");
    Check(extra.empty(), "データ表にあるのに McpDefine が無い（削除された method の残骸）: " + extra);
    for (const std::string& n : directNames)
        Check(!tableNames.count(n), "meta を直接渡した method がデータ表にも載っている: " + n);
    Check(directNames.count("describe_mcp_manifest") == 1, "describe_mcp_manifest は meta 直接渡しで登録されている");
    Check(sourceNames.size() >= 170, "McpDefine を十分に検出できている（" + std::to_string(sourceNames.size()) + " 名）");
    std::printf("  --  McpDefine %zu 名 / データ表 %zu 行 / meta 直接 %zu 名\n",
                sourceNames.size(), tableNames.size(), directNames.size());

    // ================= (b) 各エントリの妥当性 =================
    std::map<std::string, int> byEffect, byCategory;
    int deferredCount = 0;
    for (const ManifestRow& r : rows)
    {
        const std::string n = r.name;
        const McpMeta& m = r.meta;
        ++byEffect[McpEffectName(m.effect)];
        ++byCategory[m.category];
        if (m.deferred) ++deferredCount;

        Check(!m.summary.empty(), n + ": summary が空");
        Check(m.summary.size() <= 600, n + ": summary が長すぎる（" + std::to_string(m.summary.size()) + " バイト）");
        Check(!m.category.empty() && m.category != "uncategorized", n + ": category 未設定");
        Check(m.timeoutMs > 0, n + ": timeoutMs <= 0");
        Check(McpModeValid(m.mode), n + ": mode が不正 (" + m.mode + ")");
        Check(McpDryRunValid(m.dryRun), n + ": dryRun が不正 (" + m.dryRun + ")");
        Check(McpExposeValid(m.expose), n + ": expose が不正 (" + m.expose + ")");
        McpEffect e;
        Check(McpEffectFromName(McpEffectName(m.effect), e), n + ": effect が不正");
        Check(!m.keywords.empty(), n + ": keywords が空");
        for (const std::string& a : m.aliases)
            Check(a.rfind("dx12_", 0) == 0, n + ": alias は dx12_ で始まる旧ツール名 (" + a + ")");
        if (m.effect == McpEffect::Read)
            Check(m.idempotent, n + ": read なのに idempotent=false");
        if (!m.group.empty()) Check(!m.target.empty(), n + ": group があるのに target が空");

        std::set<std::string> seenParams;
        for (const McpParam& p : m.params)
        {
            Check(!p.name.empty(), n + ": 名前の無い引数");
            Check(seenParams.insert(p.name).second, n + ": 引数が重複 " + p.name);
            Check(McpParamTypeValid(p.type), n + "." + p.name + ": 型が不正 (" + p.type + ")");
            if (p.type == "enum") Check(!p.enumValues.empty(), n + "." + p.name + ": enum なのに有効値が無い");
            if (p.hasMin && p.hasMax) Check(p.min <= p.max, n + "." + p.name + ": min > max");
            Check(!p.enforce, n + "." + p.name + ": データ表由来の引数は enforce=false（既存クライアントを壊さない）");
            Check(p.desc.size() <= 400, n + "." + p.name + ": desc が長すぎる");
        }
    }
    // M5: dryRun プレビュー / ジャーナルの対象一覧とデータ表の整合。
    //   一覧の method は（エンジンに直接登録する journal_restore を除いて）データ表に実在し、Read ではない。
    //   guarded の method は表の effect から取れる（エンジン側ゲートの対象＝TS 側 catalog の GUARDED と同じ 11 件）。
    {
        std::set<std::string> directSafety = {"journal_restore"};
        for (const std::string& n : McpPreviewMethods())
        {
            if (directSafety.count(n)) continue;
            Check(metaOf.count(n) == 1, "M5: プレビュー対象 " + n + " がデータ表に無い");
            if (metaOf.count(n)) Check(metaOf[n]->effect != McpEffect::Read, "M5: プレビュー対象 " + n + " が read（read は dryRun を無視して実行する）");
        }
        for (const std::string& n : McpJournalMethods())
        {
            if (directSafety.count(n)) continue;
            Check(metaOf.count(n) == 1, "M5: journal 対象 " + n + " がデータ表に無い");
            if (metaOf.count(n))
                Check(metaOf[n]->effect == McpEffect::WriteFile || metaOf[n]->effect == McpEffect::Guarded || n == "save_scene",
                      "M5: journal 対象 " + n + " はファイルを書く method（write_file / guarded）");
        }
        Check(McpNameIn(McpPreviewMethods(), "delete_asset") && McpNameIn(McpJournalMethods(), "delete_asset"), "M5: delete_asset は preview + journal");
        std::set<std::string> guarded;
        for (const ManifestRow& r : rows) if (r.meta.effect == McpEffect::Guarded) guarded.insert(r.name);
        const std::set<std::string> expectGuarded = {"build_game", "delete_asset", "eval_lua", "git_checkout", "git_commit", "git_fetch",
                                                     "git_merge", "git_merge_abort", "git_pull", "git_push", "net_launch_test_client"};
        Check(guarded == expectGuarded, "M5: guarded の method 一覧が想定の 11 件（エンジン側ゲートの対象）");
        std::printf("  --  M5: preview %zu / journal %zu / guarded %zu\n", McpPreviewMethods().size(), McpJournalMethods().size(), guarded.size());
    }
    // 規則の下限（判定の規則が壊れていないことの粗い確認）
    auto effectOf = [&](const char* n) -> std::string { return metaOf.count(n) ? McpEffectName(metaOf[n]->effect) : "?"; };
    Check(effectOf("get_ssao") == "read" && effectOf("set_ssao") == "write_setting", "effect: get_ssao=read / set_ssao=write_setting");
    Check(effectOf("git_push") == "guarded" && effectOf("git_status") == "read", "effect: git_push=guarded / git_status=read");
    Check(effectOf("eval_lua") == "guarded" && effectOf("delete_asset") == "guarded", "effect: eval_lua / delete_asset = guarded");
    Check(effectOf("play") == "runtime" && effectOf("step_frames") == "runtime", "effect: play / step_frames = runtime");
    Check(effectOf("set_transform") == "write_scene" && effectOf("create_lua_component") == "write_file", "effect: set_transform=write_scene / create_lua_component=write_file");
    Check(effectOf("look_at") == "write_scene" && effectOf("snap_to_ground") == "write_scene", "effect: look_at / snap_to_ground = write_scene（Transform を書き換える）");
    Check(metaOf.count("set_ssao") && metaOf["set_ssao"]->group == "render_setting" && metaOf["set_ssao"]->target == "ssao",
          "render_setting の group / target");
    Check(metaOf.count("play") && metaOf["play"]->deferred && metaOf.count("create_entity") && metaOf["create_entity"]->deferred,
          "deferred: play / create_entity");
    Check(metaOf.count("ping") && !metaOf["ping"]->deferred, "ping は deferred でない");
    Check(metaOf.count("terrain_erode") && metaOf["terrain_erode"]->timeoutMs == 60000, "timeoutMs は TIMEOUT_BY_METHOD 由来（terrain_erode=60000）");
    std::printf("  --  effect:");
    for (const auto& kv : byEffect) std::printf(" %s=%d", kv.first.c_str(), kv.second);
    std::printf(" / category %zu 種 / deferred %d 件\n", byCategory.size(), deferredCount);

    // 申告表（paramSpec）と表の引数名の食い違い
    //   - 表の引数はすべて申告表にある（親.子は親も可）。
    //   - 申告表のキーはすべて表にある。ただし get|set 共有ハンドラの get_ 側（read）は set 用のキー表を持つので除く。
    //   - 申告表が X マクロ生成（literalSpec=false）のもの（set_post_process / set_ssao）は突き合わせない。
    for (const std::string& n : sourceNames)
    {
        const SourceDefine& d = defineOf[n];
        if (!d.literalSpec || !metaOf.count(n)) continue;
        const McpMeta& m = *metaOf[n];
        std::set<std::string> declared, inTable;
        for (const auto& one : Split(d.spec, ','))
            declared.insert(one.substr(0, one.find(':')));
        for (const McpParam& p : m.params) inTable.insert(p.name);
        const bool sharedRead = d.names.size() > 1 && m.effect == McpEffect::Read;
        std::string a, b;
        for (const auto& k : inTable)
        {
            const size_t dot = k.find('.');
            if (!declared.count(k) && !(dot != std::string::npos && declared.count(k.substr(0, dot))))
                a += (a.empty() ? "" : ", ") + k;
        }
        if (!sharedRead)
            for (const auto& k : declared) if (!inTable.count(k)) b += (b.empty() ? "" : ", ") + k;
        Check(a.empty(), n + ": データ表の引数が申告表（McpDefine 第 2 引数）に無い → " + a);
        Check(b.empty(), n + ": 申告表のキーがデータ表の引数に無い → " + b + "  （ApplicationMcpManifestData.inc へ足すこと）");
    }

    // ================= (d) McpErr のコード =================
    {
        bool ok = false;
        const std::string h = ReadFile(DX12E_INTERNAL_H, ok);
        if (!ok) { std::printf("SKIP: ApplicationInternal.h を開けない\n"); return 0; }
        const size_t ns = h.find("namespace McpErr");
        const size_t open = h.find('{', ns);
        const size_t close = h.find("\n}", open);
        Check(ns != std::string::npos && open != std::string::npos && close != std::string::npos, "namespace McpErr が見つかる");
        const std::string body = h.substr(open, close - open);
        std::map<std::string, int> codes;
        std::map<int, std::string> byValue;
        size_t p = 0;
        while ((p = body.find("constexpr int", p)) != std::string::npos)
        {
            p += 13;
            while (p < body.size() && body[p] == ' ') ++p;
            std::string name;
            while (p < body.size() && (std::isalnum(static_cast<unsigned char>(body[p])) || body[p] == '_')) name += body[p++];
            const size_t eq = body.find('=', p);
            if (eq == std::string::npos) break;
            const int v = std::atoi(body.c_str() + eq + 1);
            codes[name] = v;
            if (!byValue.emplace(v, name).second)
                Check(false, "McpErr の値が重複: " + name + " と " + byValue[v] + " が " + std::to_string(v));
        }
        // 既存の値は動かさない（Node 側 / 旧クライアントが数値で分類している）
        const std::map<std::string, int> expected = {
            {"NotFound", 1}, {"InvalidParam", 2}, {"ModeConflict", 3}, {"StaleScene", 4}, {"UnknownComponent", 6},
            {"Internal", 7}, {"UnknownMethod", 8}, {"Busy", 9}, {"Unsupported", 10}, {"Guarded", 11},
            {"Cancelled", 12}, {"ModalOpen", 13}, {"FileIo", 14}};
        for (const auto& kv : expected)
            Check(codes.count(kv.first) && codes[kv.first] == kv.second,
                  "McpErr::" + kv.first + " は " + std::to_string(kv.second) + " のまま");
        Check(codes.size() == expected.size(), "McpErr の要素数が想定と違う（" + std::to_string(codes.size()) + "）: 新しいコードは設計書 §4.3.2 と本テストへ足すこと");
    }

    std::printf("%s: %d checks / %zu methods / %d failures\n",
                g_failures == 0 ? "OK" : "NG", g_checks, tableNames.size(), g_failures);
    return g_failures == 0 ? 0 : 1;
#endif
}
