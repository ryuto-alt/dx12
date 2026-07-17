#pragma once
//
// VFS 経由でファイルを解決する Assimp::IOSystem（spec §9.3）。
//   importer.SetIOHandler(new VfsIOSystem()) を呼ぶことで、importer.ReadFile() が
//   .gltf / 隣の .bin / .mtl / 外部テクスチャをすべて VFS（pak or ディスク）から読む。
//   ゲームモード: pak から復号 + 展開。ディスクモード: AssetsDir のルーズファイル（バイト等価）。
//
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>

namespace dx12e
{

// VFS 経由でファイル I/O を行う Assimp IOSystem。
// Importer が SetIOHandler 経由で所有権を持ち、自身の dtor で delete する。
class VfsIOSystem : public Assimp::IOSystem
{
public:
    VfsIOSystem() = default;
    ~VfsIOSystem() override = default;

    // vfs::ExistsAbs(file)、または fs::exists なら true。
    bool Exists(const char* pFile) const override;

    // VFS のキーは常に '/' 区切り。
    char getOsSeparator() const override { return '/'; }

    // file のバイト列を VFS から読んで VfsIOStream を返す。読み取り専用（"rb"）。
    // VFS が空を返したらディスクから直接フォールバック（エディタ挙動を維持）。
    Assimp::IOStream* Open(const char* pFile, const char* pMode = "rb") override;

    void Close(Assimp::IOStream* pFile) override;
};

} // namespace dx12e
