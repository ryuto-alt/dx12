#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

struct BoneNode
{
    std::string name;
    i32 parentIndex = -1;
    DirectX::XMFLOAT4X4 inverseBindPose;
    DirectX::XMFLOAT4X4 localBindPose;

    // 「ボーンではない祖先ノード」の変換をまとめたもの（行ベクトル規約）。
    // glTF の "Z_UP" / "Armature"、Collada/FBX の軸変換ノードのように、スケルトンの
    // 外側に置かれた変換ノードは骨として現れないので、ここへ畳み込んでおき
    // ComputeGlobalMatrices が local の直後に掛ける（＝失われないようにする）。
    // 大半のモデルは単位行列なので hasPreTransform=false で完全に従来どおりの計算になる。
    DirectX::XMFLOAT4X4 preTransform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool                hasPreTransform = false;

    // localBindPose を TRS へ分解したもの（Skeleton::AddBone が自動で埋める）。
    // AnimPose がトラックの無いボーンを埋めるのに使う。行列を毎フレーム分解する
    // コストを避けるためのキャッシュであり、S*R*T で localBindPose を再構成できる。
    // 分解できない（せん断を含む）行列だと bindDecomposed=false になる。
    DirectX::XMFLOAT3 bindT{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT4 bindR{0.0f, 0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 bindS{1.0f, 1.0f, 1.0f};
    bool bindDecomposed = true;
};

class Skeleton
{
public:
    static constexpr u32 kMaxBones = 256;

    // localBindPose から bindT/bindR/bindS を計算して追加する。
    void AddBone(BoneNode bone);
    i32  FindBoneIndex(std::string_view name) const;
    u32  GetBoneCount() const { return static_cast<u32>(m_bones.size()); }

    // 親が子より前に並んでいるか（FK の単一ループが前提にしている性質。
    // JPH::Skeleton::AreJointsCorrectlyOrdered と同趣旨）。
    // false なら ComputeGlobalMatrices が「まだ計算していない親」を読んでしまう。
    bool AreBonesCorrectlyOrdered() const;

    // localBindPose の TRS 分解に失敗したボーンがあるか（AddBone が検証済み）。
    // true のとき、トラックの無いボーンのバインドポーズが元の行列と一致しない。
    bool HasUndecomposableBind() const { return m_hasUndecomposableBind; }

    // ボーン名の部分一致検索（フット IK のボーン自動特定用。大文字小文字を無視）。
    // 複数一致したら「最初に見つかったもの」。見つからなければ -1。
    i32 FindBoneIndexContaining(std::string_view fragment) const;

    const BoneNode&              GetBone(u32 index) const { return m_bones[index]; }
    const std::vector<BoneNode>& GetBones() const         { return m_bones; }

private:
    std::vector<BoneNode>                m_bones;
    std::unordered_map<std::string, i32> m_boneIndexMap;
    bool                                 m_hasUndecomposableBind = false;
};

} // namespace dx12e
