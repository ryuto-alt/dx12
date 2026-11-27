// アイコンの種別表の Graphite 専用の色の取得口（W1-A6）。
// ThemeGraphite.gen.h は重ヘッダから隠す方針なので、値を直接読むのはこの .cpp だけにする。
#include "editor/IconTable.h"
#include "editor/ThemeGraphite.gen.h"

namespace dx12e::icons
{

const ImVec4* GraphiteKindTint(int slot)
{
    switch (slot)
    {
    case 0: return &theme::gg::kGgSpec.kind.material;
    case 1: return &theme::gg::kGgSpec.kind.texture;
    default: return nullptr;
    }
}

} // namespace dx12e::icons
