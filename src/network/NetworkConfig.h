#pragma once
#include "core/Types.h"
#include "network/NetworkTypes.h"

#include <string>

namespace dx12e {

// プロジェクト単位のネットワーク設定。assets/network.json に保存(SceneFlowと同じ置き場所/流儀)。
// フェーズ⑨で NetworkPanel(エディタ設定窓) から編集される想定。フェーズ②時点では最小フィールドのみ。
struct NetworkConfig
{
    u32 tickRate     = 60;                  // シム更新Hz(物理の固定ステップと同位相にする想定)
    u32 snapshotRate = 20;                  // スナップショット送信Hz(フェーズ⑤で使用)
    u32 maxPlayers   = 8;
    u16 defaultPort  = kDefaultNetworkPort;

    bool Load(const std::string& path);
    bool LoadFromString(const std::string& jsonStr);
    bool Save(const std::string& path) const;
};

} // namespace dx12e
