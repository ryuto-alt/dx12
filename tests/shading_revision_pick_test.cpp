// ShadingRevisionPick.h（水 / 植生 / VG が V2 の .cso を選ぶ純ロジック）のテスト。GPU 不要。FU-E-25。
#include "renderer/ShadingRevisionPick.h"

#include <cstdio>
#include <set>
#include <string>

using namespace dx12e::shadingpick;

namespace { int g_failures = 0; }
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_failures; } } while (0)

int main()
{
    std::set<std::wstring> files = {L"A_PS.cso", L"APhys_PS.cso", L"AV2_PS.cso", L"APhysV2_PS.cso"};
    int calls = 0;
    const auto exists = [&](const std::wstring& n) { ++calls; return files.count(n) > 0; };

    // V1: 存在確認を呼ばず V1 の名前（既定の経路は変わらない）
    { calls = 0; auto p = PickPs(false, false, L"A_PS.cso", L"APhys_PS.cso", L"AV2_PS.cso", L"APhysV2_PS.cso", exists);
      CHECK(calls == 0); CHECK(p.name == L"A_PS.cso"); CHECK(!p.usedV2); CHECK(!p.fellBack); }
    { calls = 0; auto p = PickPs(false, true, L"A_PS.cso", L"APhys_PS.cso", L"AV2_PS.cso", L"APhysV2_PS.cso", exists);
      CHECK(calls == 0); CHECK(p.name == L"APhys_PS.cso"); CHECK(!p.usedV2); CHECK(!p.fellBack); }
    // V2 があれば V2（phys の選択も）
    { auto p = PickPs(true, false, L"A_PS.cso", L"APhys_PS.cso", L"AV2_PS.cso", L"APhysV2_PS.cso", exists);
      CHECK(p.name == L"AV2_PS.cso"); CHECK(p.usedV2); CHECK(!p.fellBack); }
    { auto p = PickPs(true, true, L"A_PS.cso", L"APhys_PS.cso", L"AV2_PS.cso", L"APhysV2_PS.cso", exists);
      CHECK(p.name == L"APhysV2_PS.cso"); CHECK(p.usedV2); CHECK(!p.fellBack); }
    // V2 が無ければ V1 へ縮退（fellBack）
    files.erase(L"AV2_PS.cso"); files.erase(L"APhysV2_PS.cso");
    { auto p = PickPs(true, false, L"A_PS.cso", L"APhys_PS.cso", L"AV2_PS.cso", L"APhysV2_PS.cso", exists);
      CHECK(p.name == L"A_PS.cso"); CHECK(!p.usedV2); CHECK(p.fellBack); }
    { auto p = PickPs(true, true, L"A_PS.cso", L"APhys_PS.cso", L"AV2_PS.cso", L"APhysV2_PS.cso", exists);
      CHECK(p.name == L"APhys_PS.cso"); CHECK(!p.usedV2); CHECK(p.fellBack); }
    // 実在しないパス / 空パスは例外なしで偽
    CHECK(!CsoFileExists(L"no_such_dir/no_such_file.cso"));
    CHECK(!CsoFileExists(L""));

    std::printf(g_failures ? "ShadingRevisionPickTests: %d failure(s)\n" : "ShadingRevisionPickTests: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
