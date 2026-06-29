#pragma once
#include <cstdint>
#include <string>

namespace dx12e {

// MCP リクエストの「遅延応答」相関情報。
// 生成/モデルロード/シーンロード/モード遷移など GPU を伴う処理はフレーム境界で実行されるため、
// 受信時には結果(本物の entity id 等)がまだ分からない。この構造体を保留キューへ持たせておき、
// 実行された後に McpBridge::SendToClient(client, ...) で本物の結果を送り返す。
//
// client == 0 は「MCP 由来でない(=エディタ UI からの操作)」を意味し、応答は送らない。
struct McpDeferred
{
    uint64_t    client = 0;       // McpBridge のクライアントトークン(= SOCKET の値)
    long long   requestId = 0;    // リクエストの id。Node クライアントからは常に正整数
    std::string idempotencyKey;   // 任意。create/spawn の再試行重複防止に使う
};

} // namespace dx12e
