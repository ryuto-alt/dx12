#pragma once

// Graphite Gauge の製図ペンのアイコンを ImDrawList で描く（W1-A6）。
// 頂点表は GgIcons.gen.h（tools/theme/gen_gg_icons.py が assets/editor/icons/gg_icons.txt から生成）。
// 16px グリッド、線幅 1.25、square cap、miter join。ImGui のフォントもテクスチャも使わない
// （折れ線を AddPolyline で描くだけなので DPI に関係なく鮮明で、色は呼び出し側が決める）。

#include "editor/GgIcons.gen.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

namespace dx12e::gg_icons
{

// id（"i-mesh" 等）から探す。無ければ nullptr。kIcons は id の昇順。
inline const Icon* Find(std::string_view id)
{
    int lo = 0, hi = kIconCount - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        const int c = std::strncmp(kIcons[mid].id, id.data(), id.size());
        const int cmp = c != 0 ? c : (kIcons[mid].id[id.size()] == '\0' ? 0 : 1);
        if (cmp == 0) return &kIcons[mid];
        if (cmp < 0) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

// center を中心に size（物理 px の一辺）で描く。線幅は size/16 に比例（最小 1px）。
inline void Draw(ImDrawList* dl, const Icon* ic, ImVec2 center, float size, ImU32 col)
{
    if (!dl || !ic) return;
    const float k = size / ic->vb;
    const ImVec2 org(center.x - size * 0.5f, center.y - size * 0.5f);
    std::vector<ImVec2> buf;
    for (int pi = 0; pi < ic->pathCount; ++pi)
    {
        const Path& p = kPaths[ic->firstPath + pi];
        buf.clear();
        for (int i = 0; i < p.count; ++i)
            buf.push_back(ImVec2(org.x + kPts[p.first + i][0] * k, org.y + kPts[p.first + i][1] * k));
        if (p.fill)
        {
            if (buf.size() >= 3) dl->AddConcavePolyFilled(buf.data(), static_cast<int>(buf.size()), col);
            continue;
        }
        const float w = std::max(1.0f, p.w * k);
        if (!p.closed && buf.size() >= 2)
        {
            // square cap: 両端を線幅の半分だけ延ばす
            auto ext = [&](ImVec2& a, const ImVec2& b)
            {
                const float dx = a.x - b.x, dy = a.y - b.y;
                const float l = std::sqrt(dx * dx + dy * dy);
                if (l > 1e-6f) { a.x += dx / l * w * 0.5f; a.y += dy / l * w * 0.5f; }
            };
            ext(buf.front(), buf[1]);
            ext(buf.back(), buf[buf.size() - 2]);
        }
        dl->AddPolyline(buf.data(), static_cast<int>(buf.size()), col, p.closed ? ImDrawFlags_Closed : ImDrawFlags_None, w);
    }
}

inline void Draw(ImDrawList* dl, std::string_view id, ImVec2 center, float size, ImU32 col)
{
    Draw(dl, Find(id), center, size, col);
}

} // namespace dx12e::gg_icons
