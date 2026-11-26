#pragma once
// ShadingRevisionPick.h — shadingRevision=2 のとき、V2 の .cso（*V2_PS.cso）を選ぶ純ロジック（FU-E-25）。
//   ApplicationPipeline.cpp の FwdPsNameRev と同じ流儀を、水・植生・仮想ジオメトリ（VG）が共有するための小さな窓口。
//   V1（v2=false）のときは V1 の名前をそのまま返す＝既定の経路は 1 バイトも変わらない。
//   V2 の .cso が無い（ビルドし忘れ）ときは V1 へ縮退する（fellBack = true。呼び出し側が警告を出す）。
//   物理ライティング単位（phys）は V1 / V2 とも Phys 版を選ぶ（V1 の Phys が無いときの縮退は呼び出し側の従来の扱いのまま）。
#include <filesystem>
#include <string>
#include <system_error>

namespace dx12e::shadingpick
{
struct Pick
{
    std::wstring name;        // 読む .cso のファイル名
    bool         usedV2 = false;   // V2 の .cso を選べた
    bool         fellBack = false; // V2 を求めたが無かったので V1 へ縮退した
};

// exists(name) -> bool: .cso が実在するか（呼び出し側が渡す。テストでは表を渡す）。
template <class ExistsFn>
inline Pick PickPs(bool v2, bool physical, const wchar_t* v1, const wchar_t* v1Phys, const wchar_t* v2Name, const wchar_t* v2Phys, ExistsFn exists)
{
    Pick p;
    if (v2)
    {
        const wchar_t* want = physical ? v2Phys : v2Name;
        if (exists(std::wstring(want)))
        {
            p.name = want;
            p.usedV2 = true;
            return p;
        }
        p.fellBack = true;
    }
    p.name = physical ? v1Phys : v1;
    return p;
}

// 実ファイルの存在（サイズ 0 は無いものとして扱う。例外を投げない）。
inline bool CsoFileExists(const std::wstring& path)
{
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::filesystem::path(path), ec);
    return !ec && sz > 0;
}
} // namespace dx12e::shadingpick
