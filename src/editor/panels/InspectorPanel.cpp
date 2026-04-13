#include "editor/panels/InspectorPanel.h"
#include "editor/EditorContext.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "renderer/Material.h"
#include "renderer/Mesh.h"
#include "audio/AudioSystem.h"
#include "physics/PhysicsDebugRenderer.h"
#include "core/GameClock.h"
#include "scene/Scene.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "animation/SkinningBuffer.h"

#include <filesystem>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <DirectXMath.h>

namespace dx12e
{

void InspectorPanel::Render(entt::registry& reg,
                            EditorContext& ctx,
                            Camera* camera,
                            AudioSystem* audioSystem,
                            PhysicsDebugRenderer* physicsDebugRenderer,
                            bool& physicsDebugDraw,
                            bool& useVsync,
                            i32& shadowQualityIndex,
                            u32& shadowMapSize,
                            bool& shadowMapDirty,
                            GameClock* clock,
                            Scene* /*scene*/)
{
    ImGui::Begin("\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc");  // Inspector

    // --- Selected Entity properties ---
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity))
    {
        // NameTag
        if (reg.all_of<NameTag>(ctx.selectedEntity))
        {
            auto& tag = reg.get<NameTag>(ctx.selectedEntity);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.39f, 0.58f, 0.93f, 1.0f));
            ImGui::Text("%s", tag.name.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        // Transform
        if (reg.all_of<Transform>(ctx.selectedEntity))
        {
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto& t = reg.get<Transform>(ctx.selectedEntity);
                ImGui::DragFloat3("Position", &t.position.x, 0.1f);
                ImGui::DragFloat3("Rotation", &t.rotation.x, 1.0f);
                ImGui::DragFloat3("Scale",    &t.scale.x,    0.01f);
            }
        }

        // MeshRenderer
        if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
        {
            if (ImGui::CollapsingHeader("MeshRenderer"))
            {
                auto& r = reg.get<MeshRenderer>(ctx.selectedEntity);
                ImGui::Text("Meshes: %d", static_cast<int>(r.meshes.size()));
                ImGui::Text("Materials: %d", static_cast<int>(r.materials.size()));
            }
        }

        // SkeletalAnimation
        if (reg.all_of<SkeletalAnimation>(ctx.selectedEntity))
        {
            auto& skelAnim = reg.get<SkeletalAnimation>(ctx.selectedEntity);
            if (skelAnim.animator && ImGui::CollapsingHeader("SkeletalAnimation"))
            {
                ImGui::Text("Bones: %d",
                    static_cast<int>(skelAnim.skeleton ? skelAnim.skeleton->GetBoneCount() : 0));
                ImGui::Text("Clips: %d", static_cast<int>(skelAnim.clips.size()));

                for (i32 i = 0; i < static_cast<i32>(skelAnim.clips.size()); ++i)
                {
                    const auto& clip = skelAnim.clips[i];
                    std::string label = clip->GetName().empty()
                        ? ("Clip " + std::to_string(i))
                        : clip->GetName();
                    if (ImGui::Selectable(label.c_str()))
                        skelAnim.animator->CrossFadeTo(clip.get(), 0.3f);
                }
            }
        }

        // NodeAnimation
        if (reg.all_of<NodeAnimationComp>(ctx.selectedEntity))
        {
            auto& nodeAnim = reg.get<NodeAnimationComp>(ctx.selectedEntity);
            if (nodeAnim.nodeAnimator && ImGui::CollapsingHeader("NodeAnimation"))
            {
                ImGui::Text("Clips: %d", static_cast<int>(nodeAnim.clips.size()));
                for (i32 i = 0; i < static_cast<i32>(nodeAnim.clips.size()); ++i)
                {
                    const auto& clip = nodeAnim.clips[i];
                    std::string label = clip->GetName().empty()
                        ? ("Clip " + std::to_string(i))
                        : clip->GetName();
                    if (ImGui::Selectable(label.c_str()))
                        nodeAnim.nodeAnimator->CrossFadeTo(clip.get(), 0.3f);
                }
            }
        }

        // GridPlane
        if (reg.all_of<GridPlane>(ctx.selectedEntity))
        {
            if (ImGui::CollapsingHeader("GridPlane"))
            {
                auto& gp = reg.get<GridPlane>(ctx.selectedEntity);
                ImGui::Checkbox("Enabled", &gp.enabled);
            }
        }

        // PointLight
        if (reg.all_of<PointLight>(ctx.selectedEntity))
        {
            if (ImGui::CollapsingHeader("PointLight"))
            {
                auto& pl = reg.get<PointLight>(ctx.selectedEntity);
                ImGui::ColorEdit3("Color", &pl.color.x);
                ImGui::DragFloat("Intensity", &pl.intensity, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("Range", &pl.range, 0.5f, 0.0f, 500.0f);
            }
        }

        // DirectionalLight
        if (reg.all_of<DirectionalLight>(ctx.selectedEntity))
        {
            if (ImGui::CollapsingHeader("DirectionalLight"))
            {
                auto& dl = reg.get<DirectionalLight>(ctx.selectedEntity);
                ImGui::DragFloat3("Direction", &dl.direction.x, 0.01f);
                ImGui::ColorEdit3("Color", &dl.color.x);
                ImGui::DragFloat("Intensity", &dl.intensity, 0.1f, 0.0f, 100.0f);
            }
        }

        // CameraComponent
        if (reg.all_of<CameraComponent>(ctx.selectedEntity))
        {
            if (ImGui::CollapsingHeader("Camera"))
            {
                auto& cam = reg.get<CameraComponent>(ctx.selectedEntity);
                ImGui::DragFloat("FOV", &cam.fovDegrees, 1.0f, 1.0f, 179.0f);
                ImGui::DragFloat("Near", &cam.nearClip, 0.01f, 0.001f, 100.0f);
                ImGui::DragFloat("Far", &cam.farClip, 10.0f, 1.0f, 100000.0f);
                ImGui::Checkbox("Active", &cam.isActive);
            }
        }

        // --- Physics ---
        {
            bool hasRb = reg.all_of<RigidBody>(ctx.selectedEntity);
            if (ImGui::Checkbox("Physics", &hasRb))
            {
                if (hasRb)
                {
                    // Add physics: auto collider + RigidBody
                    if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
                    {
                        auto* mr = &reg.get<MeshRenderer>(ctx.selectedEntity);
                        auto* tf = &reg.get<Transform>(ctx.selectedEntity);
                        std::vector<DirectX::XMFLOAT3> allPoints;
                        for (const auto* mesh : mr->meshes)
                        {
                            if (!mesh) continue;
                            for (const auto& p : mesh->GetPositions())
                                allPoints.push_back({
                                    p.x * tf->scale.x,
                                    p.y * tf->scale.y,
                                    p.z * tf->scale.z });
                        }
                        constexpr size_t kMax = 256;
                        if (allPoints.size() > kMax)
                        {
                            size_t step = allPoints.size() / kMax;
                            std::vector<DirectX::XMFLOAT3> sampled;
                            for (size_t i = 0; i < allPoints.size() && sampled.size() < kMax; i += step)
                                sampled.push_back(allPoints[i]);
                            allPoints = std::move(sampled);
                        }
                        if (!allPoints.empty())
                        {
                            ConvexHullCollider col;
                            col.points = std::move(allPoints);
                            reg.emplace_or_replace<ConvexHullCollider>(ctx.selectedEntity, std::move(col));
                        }
                    }
                    reg.emplace_or_replace<RigidBody>(ctx.selectedEntity);
                }
                else
                {
                    reg.remove<RigidBody>(ctx.selectedEntity);
                    reg.remove<ConvexHullCollider>(ctx.selectedEntity);
                    reg.remove<BoxCollider>(ctx.selectedEntity);
                    reg.remove<SphereCollider>(ctx.selectedEntity);
                    reg.remove<CapsuleCollider>(ctx.selectedEntity);
                }
            }

            if (reg.all_of<RigidBody>(ctx.selectedEntity))
            {
                auto& rb = reg.get<RigidBody>(ctx.selectedEntity);

                const char* motionTypes[] = { "Static", "Kinematic", "Dynamic" };
                int motionIdx = static_cast<int>(rb.motionType);
                if (ImGui::Combo("Motion", &motionIdx, motionTypes, 3))
                    rb.motionType = static_cast<MotionType>(motionIdx);

                ImGui::DragFloat("Mass", &rb.mass, 0.5f, 0.0f, 10000.0f);
                ImGui::DragFloat("Friction", &rb.friction, 0.01f, 0.0f, 2.0f);
                ImGui::DragFloat("Bounce", &rb.restitution, 0.01f, 0.0f, 1.0f);
                ImGui::Checkbox("Gravity", &rb.useGravity);
            }
        }

        // --- Material (PBR) ---
        if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
        {
            auto& mr = reg.get<MeshRenderer>(ctx.selectedEntity);
            if (!mr.meshes.empty() && mr.meshes[0] && mr.meshes[0]->GetMaterial())
            {
                const auto* mat = mr.meshes[0]->GetMaterial();
                if (ImGui::CollapsingHeader("Material"))
                {
                    if (mr.overrideMetallic < 0.0f)
                        mr.overrideMetallic = mat->defaultMetallic;
                    if (mr.overrideRoughness < 0.0f)
                        mr.overrideRoughness = mat->defaultRoughness;

                    ImGui::SliderFloat("Metallic", &mr.overrideMetallic, 0.0f, 1.0f);
                    ImGui::SliderFloat("Roughness", &mr.overrideRoughness, 0.0f, 1.0f);

                    bool hasNormal = mat->normalMapTexture != nullptr;
                    bool hasMR = mat->metalRoughnessTexture != nullptr;
                    ImGui::Text("Normal Map: %s", hasNormal ? "Yes" : "No");
                    ImGui::Text("MetalRough Map: %s", hasMR ? "Yes" : "No");
                }
            }
        }
    }
    else
    {
        ImGui::TextDisabled("\xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84");  // Select an entity
    }

    ImGui::Separator();

    // --- Camera ---
    if (ImGui::CollapsingHeader("\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9", ImGuiTreeNodeFlags_DefaultOpen))  // Camera
    {
        auto camPos = camera->GetPosition();
        ImGui::Text("%.1f, %.1f, %.1f", camPos.x, camPos.y, camPos.z);
        f32 moveSpeed = camera->GetMoveSpeed();
        if (ImGui::SliderFloat("\xe9\x80\x9f\xe5\xba\xa6", &moveSpeed, 1.0f, 50.0f))  // Speed
            camera->SetMoveSpeed(moveSpeed);
    }

    // --- Shadow quality ---
    if (ImGui::CollapsingHeader("\xe3\x82\xb7\xe3\x83\xa3\xe3\x83\x89\xe3\x82\xa6"))  // Shadow
    {
        const char* qualities[] = {"1024 (Low)", "2048 (Medium)", "4096 (High)", "8192 (Ultra)"};
        const u32 sizes[] = {1024, 2048, 4096, 8192};
        if (ImGui::Combo("\xe8\xa7\xa3\xe5\x83\x8f\xe5\xba\xa6", &shadowQualityIndex, qualities, 4))  // Resolution
        {
            shadowMapSize = sizes[shadowQualityIndex];
            shadowMapDirty = true;
        }
        ImGui::Text("%ux%u", shadowMapSize, shadowMapSize);
    }

    // --- Audio ---
    if (ImGui::CollapsingHeader("\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xaa"))  // Audio
    {
        f32 masterVol = audioSystem->GetMasterVolume();
        f32 bgmVol    = audioSystem->GetBGMVolume();
        f32 sfxVol    = audioSystem->GetSFXVolume();
        if (ImGui::SliderFloat("\xe3\x83\x9e\xe3\x82\xb9\xe3\x82\xbf\xe3\x83\xbc", &masterVol, 0.0f, 1.0f))
            audioSystem->SetMasterVolume(masterVol);
        if (ImGui::SliderFloat("BGM", &bgmVol, 0.0f, 1.0f))
            audioSystem->SetBGMVolume(bgmVol);
        if (ImGui::SliderFloat("SE", &sfxVol, 0.0f, 1.0f))
            audioSystem->SetSFXVolume(sfxVol);

        const auto& bgmList = audioSystem->GetBGMList();
        for (const auto& bgm : bgmList)
        {
            std::string fn = std::filesystem::path(bgm).filename().string();
            ImGui::PushID(bgm.c_str());
            if (ImGui::Button("\xe2\x96\xb6")) audioSystem->PlayBGM(bgm);
            ImGui::SameLine(); ImGui::Text("%s", fn.c_str());
            ImGui::PopID();
        }
        if (!bgmList.empty() && ImGui::Button("BGM \xe5\x81\x9c\xe6\xad\xa2"))
            audioSystem->StopBGM();

        const auto& sfxList = audioSystem->GetSFXList();
        for (const auto& sfx : sfxList)
        {
            std::string fn = std::filesystem::path(sfx).filename().string();
            ImGui::PushID(sfx.c_str());
            if (ImGui::Button("\xe2\x96\xb6")) audioSystem->PlaySFX(sfx);
            ImGui::SameLine(); ImGui::Text("%s", fn.c_str());
            ImGui::PopID();
        }
        if (!sfxList.empty() && ImGui::Button("SE \xe5\x85\xa8\xe5\x81\x9c\xe6\xad\xa2"))
            audioSystem->StopAllSFX();
    }

    // --- Settings ---
    if (ImGui::CollapsingHeader("\xe8\xa8\xad\xe5\xae\x9a"))  // Settings
    {
        ImGui::Checkbox("VSync", &useVsync);

        bool debugDraw = physicsDebugRenderer->IsEnabled();
        if (ImGui::Checkbox("Physics Debug", &debugDraw))
        {
            physicsDebugRenderer->SetEnabled(debugDraw);
            physicsDebugDraw = debugDraw;
        }
    }

    // --- Build ---
    if (ImGui::CollapsingHeader("\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // Build
    {
        if (ImGui::Button("\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // Game Build
        {
            ctx.pendingBuildGame = true;
        }
        if (ctx.buildCompleteFlash > 0.0f)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
            ImGui::Text("\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86!");  // Build complete!
            ImGui::PopStyleColor();
            ctx.buildCompleteFlash -= clock->GetDeltaTime();
        }
    }

    ImGui::End();
}

} // namespace dx12e
