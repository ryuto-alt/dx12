#pragma once
//
// CookObj.h ― 開発用の最小 OBJ ローダ（→ VgsrcData）。assimp は使わない（cooker を依存ゼロに保つため。glTF は VGSRC へ変換して渡す）。
//
//   対応: v / vt / vn / f（多角形は扇状に三角形化。負の添字可）/ usemtl（連続区間ごとに 1 セクション）。他の行は無視。
//   UV は OBJ の下原点 → 左上原点へ（v' = 1 - v）。座標はそのままエンジン空間として扱う（巻き順が逆なら flipWinding）。
//   材質: usemtl の名前ごとに既定材質を 1 つ作る（テクスチャは読まない）。
//   全ての面の全ての角に vn / vt がある場合だけ法線 / UV を持つ（欠けていれば cooker が作る / (0,0)）。
//
#include "renderer/vg/VgsrcFormat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace dx12e::vg::cook
{

// 空白区切りの浮動小数を最大 n 個読む（sscanf を避ける）。読めた個数を返す。
inline int ReadFloats(const char* s, int n, f32* out)
{
    int k = 0;
    while (k < n)
    {
        char* e = nullptr;
        const f32 v = std::strtof(s, &e);
        if (e == s) break;
        out[k++] = v;
        s = e;
    }
    return k;
}

inline VgeoError ParseObj(const std::string& text, VgsrcData& out, bool flipWinding = false, f32 scale = 1.0f)
{
    using detail::MakeErr;
    out = VgsrcData{};
    std::vector<f32> P, N, T;
    struct Corner { i64 v, t, n; };
    std::map<std::tuple<i64, i64, i64>, u32> cornerMap;
    std::vector<f32> pos, nrm, uv;
    std::vector<u32> idx;
    bool allN = true, allT = true;
    StringPool pool;
    std::map<std::string, u32> matIndex;
    std::vector<MaterialRecord> mats;
    auto matFor = [&](const std::string& name) -> u32
    {
        auto it = matIndex.find(name);
        if (it != matIndex.end()) return it->second;
        MaterialRecord m = MakeDefaultMaterial();
        m.nameOff = pool.Add(name);
        mats.push_back(m);
        matIndex.emplace(name, static_cast<u32>(mats.size() - 1));
        return static_cast<u32>(mats.size() - 1);
    };
    u32 curMat = matFor("default");
    struct Run { u32 mat; size_t firstIdx; };
    std::vector<Run> runs;
    runs.push_back(Run{curMat, 0});

    size_t line = 0;
    const char* s = text.c_str();
    const char* end = s + text.size();
    while (s < end)
    {
        ++line;
        const char* e = static_cast<const char*>(std::memchr(s, '\n', static_cast<size_t>(end - s)));
        if (!e) e = end;
        std::string ln(s, e);
        s = e < end ? e + 1 : end;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ' || ln.back() == '\t')) ln.pop_back();
        if (ln.empty() || ln[0] == '#') continue;
        const char* c = ln.c_str();
        if (std::strncmp(c, "v ", 2) == 0)
        {
            f32 xyz[3];
            if (ReadFloats(c + 2, 3, xyz) != 3) return MakeErr(Errc::BadFloat, "obj: bad v line " + std::to_string(line));
            P.push_back(xyz[0] * scale); P.push_back(xyz[1] * scale); P.push_back(xyz[2] * scale);
        }
        else if (std::strncmp(c, "vn ", 3) == 0)
        {
            f32 xyz[3];
            if (ReadFloats(c + 3, 3, xyz) != 3) return MakeErr(Errc::BadFloat, "obj: bad vn line " + std::to_string(line));
            N.push_back(xyz[0]); N.push_back(xyz[1]); N.push_back(xyz[2]);
        }
        else if (std::strncmp(c, "vt ", 3) == 0)
        {
            f32 uvv[2] = {0, 0};
            if (ReadFloats(c + 3, 2, uvv) < 1) return MakeErr(Errc::BadFloat, "obj: bad vt line " + std::to_string(line));
            T.push_back(uvv[0]); T.push_back(1.0f - uvv[1]);
        }
        else if (std::strncmp(c, "usemtl ", 7) == 0)
        {
            const u32 m = matFor(std::string(c + 7));
            if (m != curMat)
            {
                curMat = m;
                if (runs.back().firstIdx == idx.size()) runs.back().mat = m;
                else runs.push_back(Run{m, idx.size()});
            }
        }
        else if (std::strncmp(c, "f ", 2) == 0)
        {
            std::vector<u32> face;
            const char* p = c + 2;
            while (*p)
            {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p) break;
                i64 iv = 0, it = 0, in = 0;
                char* q = nullptr;
                iv = std::strtoll(p, &q, 10);
                if (q == p) return MakeErr(Errc::CountMismatch, "obj: bad f line " + std::to_string(line));
                p = q;
                bool hasT = false, hasN = false;
                if (*p == '/')
                {
                    ++p;
                    if (*p != '/') { it = std::strtoll(p, &q, 10); hasT = q != p; p = q; }
                    if (*p == '/') { ++p; in = std::strtoll(p, &q, 10); hasN = q != p; p = q; }
                }
                const i64 nP = static_cast<i64>(P.size() / 3), nT = static_cast<i64>(T.size() / 2), nN = static_cast<i64>(N.size() / 3);
                if (iv < 0) iv = nP + iv + 1;
                if (hasT && it < 0) it = nT + it + 1;
                if (hasN && in < 0) in = nN + in + 1;
                if (iv < 1 || iv > nP || (hasT && (it < 1 || it > nT)) || (hasN && (in < 1 || in > nN))) return MakeErr(Errc::ClusterInvalid, "obj: face index out of range at line " + std::to_string(line));
                if (!hasT) allT = false;
                if (!hasN) allN = false;
                const auto key = std::make_tuple(iv, hasT ? it : 0, hasN ? in : 0);
                auto found = cornerMap.find(key);
                if (found == cornerMap.end())
                {
                    const u32 vi = static_cast<u32>(pos.size() / 3);
                    pos.insert(pos.end(), &P[static_cast<size_t>(iv - 1) * 3], &P[static_cast<size_t>(iv - 1) * 3] + 3);
                    if (hasN) nrm.insert(nrm.end(), &N[static_cast<size_t>(in - 1) * 3], &N[static_cast<size_t>(in - 1) * 3] + 3); else nrm.insert(nrm.end(), {0.0f, 0.0f, 0.0f});
                    if (hasT) uv.insert(uv.end(), &T[static_cast<size_t>(it - 1) * 2], &T[static_cast<size_t>(it - 1) * 2] + 2); else uv.insert(uv.end(), {0.0f, 0.0f});
                    found = cornerMap.emplace(key, vi).first;
                }
                face.push_back(found->second);
            }
            if (face.size() < 3) continue;
            for (size_t k = 1; k + 1 < face.size(); ++k)
            {
                idx.push_back(face[0]);
                idx.push_back(flipWinding ? face[k + 1] : face[k]);
                idx.push_back(flipWinding ? face[k] : face[k + 1]);
            }
        }
    }
    if (idx.empty()) return MakeErr(Errc::CountMismatch, "obj: no faces");
    out.positions = std::move(pos);
    out.flags = 0;
    if (allN) { out.normals = std::move(nrm); out.flags |= kVgsrcHasNormals; }
    if (allT) { out.uv0 = std::move(uv); out.flags |= kVgsrcHasUV0; }
    out.indices = std::move(idx);
    // セクション: 連続区間ごと（空の区間は捨てる）
    for (size_t r = 0; r < runs.size(); ++r)
    {
        const size_t b = runs[r].firstIdx, e = (r + 1 < runs.size()) ? runs[r + 1].firstIdx : out.indices.size();
        if (e > b) out.sections.push_back(VgsrcSection{runs[r].mat, static_cast<u32>(b), static_cast<u32>(e - b), 0});
    }
    out.materials = std::move(mats);
    out.strings = pool.Data();
    return VgeoError{};
}

inline VgeoError LoadObjFile(const std::string& path, VgsrcData& out, bool flipWinding = false, f32 scale = 1.0f)
{
    std::FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return detail::MakeErr(Errc::IoError, "cannot open " + path);
    std::string text;
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return ParseObj(text, out, flipWinding, scale);
}

} // namespace dx12e::vg::cook
