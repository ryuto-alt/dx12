#pragma once
// ===========================================================================
// マニフェストのデータ表（ApplicationMcpManifestData.inc）を書くための小さな部品
// ---------------------------------------------------------------------------
// ★依存ゼロ（McpMeta.h だけ）。エンジン本体（ApplicationMcpManifest.cpp）と、エンジンをリンクしない
//   ctest（tests/mcp_manifest_test.cpp）の両方が同じデータ表を include して中身を検査できるように
//   部品だけをここへ切り出してある。
//
//   P(name, type, required, enumPipe, min, max, default, desc) … 引数 1 件
//       enumPipe は "a|b|c"、min / max / default は数値・JSON リテラルの文字列（nullptr = 無し）。
//   M(summary, keywords, category, group, target, effect, mode, timeoutMs,
//     idempotent, deferred, dryRun, aliasesPipe, params)      … method 1 件ぶんの McpMeta
//       aliasesPipe は "dx12_a|dx12_b"。
// ===========================================================================

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/mcp/McpMeta.h"

namespace dx12e
{
namespace mcpdata
{

struct ManifestRow
{
    const char* name;
    McpMeta     meta;
};
using ManifestRows = std::vector<ManifestRow>;

inline std::vector<std::string> SplitPipe(const char* pipe)
{
    std::vector<std::string> out;
    if (!pipe || !*pipe) return out;
    const std::string all(pipe);
    size_t pos = 0;
    for (;;)
    {
        const size_t bar = all.find('|', pos);
        out.push_back(all.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos));
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    return out;
}

inline McpParam P(const char* name, const char* type, bool required, const char* enumPipe,
                  const char* mn, const char* mx, const char* def, const char* desc)
{
    McpParam p;
    p.name       = name;
    p.type       = type;
    p.required   = required;
    p.enumValues = SplitPipe(enumPipe);
    if (mn)   { p.hasMin = true; p.min = std::strtod(mn, nullptr); }
    if (mx)   { p.hasMax = true; p.max = std::strtod(mx, nullptr); }
    if (def)  { p.hasDefault = true; p.defaultJson = def; }
    if (desc) p.desc = desc;
    return p;
}

inline McpMeta M(const char* summary, const char* keywords, const char* category, const char* group,
                 const char* target, McpEffect effect, const char* mode, int timeoutMs, bool idempotent,
                 bool deferred, const char* dryRun, const char* aliasesPipe, std::vector<McpParam> params)
{
    McpMeta m;
    m.summary    = summary;
    m.keywords   = keywords;
    m.category   = category;
    m.group      = group;
    m.target     = target;
    m.effect     = effect;
    m.mode       = mode;
    m.timeoutMs  = timeoutMs;
    m.idempotent = idempotent;
    m.deferred   = deferred;
    m.dryRun     = dryRun;
    m.aliases    = SplitPipe(aliasesPipe);
    m.params     = std::move(params);
    return m;
}

} // namespace mcpdata
} // namespace dx12e
