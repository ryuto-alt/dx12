#pragma once
#include "core/Types.h"

namespace dx12e {

// netId はシーン/ネットワークをまたいで安定した識別子。entt::entity(バージョン込み・
// プロセスローカル)はネットワークへ一切流さない(join/再接続でIDがずれるため)。
using NetId    = u32;
using ClientId = u16;
using NetTick  = u32;

constexpr NetId    kInvalidNetId       = 0;
constexpr ClientId kServerClientId     = 0;   // ホスト自身(サーバー内ローカルプレイヤー)
constexpr u16      kDefaultNetworkPort = 7777;

// ENet 側のチャネル番号。ch0=信頼(ハンドシェイク/スポーン/RPC)、ch1=非信頼(スナップショット/入力)。
enum class NetChannel : u8
{
    Reliable   = 0,
    Unreliable = 1,
};
constexpr u32 kNetChannelCount = 2;

// アプリケーション層パケット種別(NetBuffer 先頭 1 バイト)。
// 接続直後の HELLO は無し: ENet の Connected イベント自体が「接続してよい」の合図なので、
// サーバーはそれを受けて即 Welcome+Baseline を送る(クライアント→サーバーの事前情報が要る
// ようになったら HELLO を足す)。
enum class PacketType : u8
{
    Welcome      = 0,   // サーバー→クライアント: clientId + シーンパス
    SceneReady   = 1,   // クライアント→サーバー: シーンロード完了(Baseline適用済み)
    Baseline     = 2,   // サーバー→クライアント: 静的エンティティの netId 対応表(view順)
    Spawn        = 3,   // サーバー→クライアント: 動的エンティティ生成(SerializeEntity blob)
    Despawn      = 4,   // サーバー→クライアント: エンティティ破棄
    Snapshot     = 5,   // サーバー→クライアント: NetworkTransform 差分
    Input        = 6,   // クライアント→サーバー: InputCommand
    RpcMessage   = 7,   // 双方向: 名前付きRPC
};

} // namespace dx12e
