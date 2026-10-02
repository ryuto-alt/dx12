// 保存中の強制終了テストの補助ツール（ctest には登録しない。手順は dx12-ui-audit/SAFE_SAVE_REPORT.md）。
//
//   SceneSafeSaveTool gen <scene.json> <cells> <fillers>   … 分割保存のシーンを作る（各セルにマーカー 1 体 + 詰め物 <fillers> 体）。マーカーの y = 0
//   SceneSafeSaveTool verify <scene.json> <cells> <fillers> … 保存途中で止められたシーンを開き、完全に揃っているか確かめる
//        出力 1 行: "OK marker=<K>"（全マーカーの y が同じ K・体数が一致）/ "BAD <理由>"
//        開く前に RecoverPending が走る（SceneSerializer::Load の入口）。完全 = 「前の版」か「新しい版」のどちらか 1 つに揃っていること。

#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/ScenePartition.h"
#include "core/AtomicFile.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"

#include <entt/entt.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace dx12e;

int main(int argc, char** argv)
{
    if (argc < 5) { std::printf("usage: gen|verify <scene.json> <cells> <fillers>\n"); return 2; }
    const std::string mode = argv[1], path = argv[2];
    const int cells = std::atoi(argv[3]), fillers = std::atoi(argv[4]);

    if (mode == "gen")
    {
        Scene s;
        auto& reg = s.GetRegistry();
        for (int c = 0; c < cells; ++c)
        {
            const float cx = static_cast<float>(c) * 70.0f;   // セル 64m に 1 つずつ入る
            {
                const entt::entity e = reg.create();
                reg.emplace<NameTag>(e, NameTag{"M_" + std::to_string(c)});
                Transform t; t.position = {cx, 0.0f, 0.0f};
                reg.emplace<Transform>(e, t);
                reg.emplace<BoxCollider>(e, BoxCollider{});
            }
            for (int i = 0; i < fillers; ++i)
            {
                const entt::entity e = reg.create();
                reg.emplace<NameTag>(e, NameTag{"F_" + std::to_string(c) + "_" + std::to_string(i)});
                Transform t; t.position = {cx + static_cast<float>(i % 10), 0.0f, static_cast<float>(i / 10)};
                reg.emplace<Transform>(e, t);
                reg.emplace<BoxCollider>(e, BoxCollider{});
            }
        }
        {
            // ルート（foo.json）側にだけ置かれるライト。強さ = マーカーの y + 1（ルートとセルの版が揃っているかを見る）
            const entt::entity e = reg.create();
            reg.emplace<NameTag>(e, NameTag{"Sun"});
            reg.emplace<Transform>(e, Transform{});
            DirectionalLight dl;
            dl.intensity = 1.0f;
            reg.emplace<DirectionalLight>(e, dl);
        }
        s.SetPartitionCellSize(64.0f);
        const bool ok = SceneSerializer::Save(s, path, "");
        std::printf(ok ? "GEN OK\n" : "GEN FAIL\n");
        return ok ? 0 : 1;
    }

    // verify: GPU の無いこのツールでは Scene::Load（グリッドなど描画物の生成が要る）を使わず、エンジンの読み込みと同じ手順の前半
    // （RecoverPending → ルートの JSON パース → 分割ファイルの統合）を行って、エンティティの JSON を直接検査する。
    // （エンジン自身が開けることは、強制終了の次の起動で open_scene が確かめる。）
    namespace fs = std::filesystem;
    if (atomicfile::RecoverPending(fs::path(path).concat(".dx12txn")))
        std::printf("(recovered) ");
    std::string text;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) { std::printf("BAD cannot open\n"); return 1; }
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    nlohmann::json root = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) { std::printf("BAD root json broken (%zu bytes)\n", text.size()); return 1; }
    std::string err;
    if (!scenepart::MergePartsFromDisk(root, path, err)) { std::printf("BAD parts: %s\n", err.c_str()); return 1; }
    int markers = 0, fill = 0;
    double markerY = 0.0, sun = -1.0;
    bool uniform = true;
    if (!root.contains("entities") || !root["entities"].is_array()) { std::printf("BAD no entities\n"); return 1; }
    for (const auto& e : root["entities"])
    {
        const std::string name = e.value("name", std::string());
        if (name.rfind("M_", 0) == 0)
        {
            const double y = e.at("transform").at("position").at(1).get<double>();
            if (markers == 0) markerY = y;
            else if (std::fabs(y - markerY) > 1e-4) uniform = false;
            ++markers;
        }
        else if (name.rfind("F_", 0) == 0) ++fill;
        else if (name == "Sun")
            sun = e.contains("directionalLight") ? e["directionalLight"].value("intensity", 1.0) : 1.0;   // v2 は既定値（1.0）を省略する
    }
    if (markers != cells) { std::printf("BAD markers=%d expected=%d\n", markers, cells); return 1; }
    if (fill != cells * fillers) { std::printf("BAD fillers=%d expected=%d\n", fill, cells * fillers); return 1; }
    if (!uniform) { std::printf("BAD mixed versions (markers differ)\n"); return 1; }
    if (std::fabs(sun - (markerY + 1.0)) > 1e-3)
    {
        std::printf("BAD root/cell version mismatch (sun=%.3f marker=%.3f)\n", sun, markerY);
        return 1;
    }
    std::printf("OK marker=%d\n", static_cast<int>(std::lround(markerY)));
    return 0;
}
