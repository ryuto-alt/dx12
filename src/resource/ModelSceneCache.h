#pragma once
// ============================================================================
// モデル読み込みのディスクキャッシュ（エディタ専用・assets/.modelcache/）
//
// 1) 解析済みの aiScene: Assimp の後処理まで済んだシーンを assbin（Assimp 自前のバイナリ）で置く。
//    2 回目からは FBX / glTF の解析と後処理（法線・接線・頂点の融合）を飛ばして読むだけにする
//    （実測: Bistro の FBX 120MB で ReadFile 3.1 秒）。初回の書き出しは裏のスレッドで行う。
//    ★assbin が持たないもの: メッシュ名・埋め込みテクスチャのファイル名・シーンのメタデータ。
//      前 2 つはこのファイルに足して保存し、読んだ後に戻す。メタデータ（UnitScaleFactor）を使うのは
//      スキンメッシュの読み直しだけなので、ボーンのあるモデルはキャッシュしない。
// 2) LOD: Mesh::GenerateLods の簡略化結果（インデックス列）。ModelLoader が 1 モデルを読む間だけ
//    LodCacheScope で有効にし、Mesh がメッシュの中身（位置 + インデックス）のハッシュで引く。
//
// キーは モデルのパス + サイズ + 更新時刻 + 後処理フラグ + Assimp の版 + このファイルの形式の版。
// 更新時刻が変われば（git checkout でも）作り直すだけ。配布ゲーム（pak）では使わない。
// ============================================================================
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include "core/Types.h"

struct aiScene;
namespace Assimp { class Importer; }

namespace dx12e::modelcache
{

// 置いてあれば importer で読み、メッシュ名などを戻して返す（scene は importer が持つ）。無ければ nullptr。
const aiScene* TryLoadScene(Assimp::Importer& importer, const std::filesystem::path& modelPath, unsigned int flags);

// キャッシュしてよいシーンか（ボーン無し・メッシュあり）。
bool IsSceneCacheable(const aiScene* scene);

// 裏のスレッドで書き出す。scene の所有権を受け取る（importer.GetOrphanedScene() したもの）。終わったら delete する。
void SaveSceneAsync(aiScene* scene, const std::filesystem::path& modelPath, unsigned int flags);

// 裏の書き出しが全部終わるまで待つ（テスト・終了時用）。
void WaitPendingSaves();

// 1 モデルの読み込みの間だけ LOD キャッシュを有効にする（同じスレッドの Mesh::GenerateLods が引く）。
class LodCacheScope
{
public:
    explicit LodCacheScope(const std::filesystem::path& modelPath);
    ~LodCacheScope();   // 新しく組んだ LOD があれば書き出す（使われた分だけ残す）
    LodCacheScope(const LodCacheScope&) = delete;
    LodCacheScope& operator=(const LodCacheScope&) = delete;

    struct Impl;
private:
    std::unique_ptr<Impl> m_impl;
};

// Mesh::GenerateLods から。有効な LodCacheScope が無ければ何もしない（false）。
// lods = LOD1 以降のインデックス列（LOD0 は含まない）。
bool FindLods(uint64_t key, std::vector<std::vector<u32>>& lods);
void StoreLods(uint64_t key, const std::vector<std::vector<u32>>& lods);
bool LodCacheActive();

} // namespace dx12e::modelcache
