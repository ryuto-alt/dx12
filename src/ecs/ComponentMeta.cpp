// コア部品のフィールド反映（entt::meta）の実体。
//
// entt::meta_factory<T>{}.type("T").data<&T::field>("field") で登録する。
// 文字列名は entt 3.16 がそのまま保持し、meta_data::name() で読み戻せる。
// ここに登録した「恒久コア部品」は、Inspector 自動 UI とシリアライズの単一ソースになる。
//
// 注: ジャンル固有部品（Gimmick / Trigger 等）はここに登録しない。
//     それらは Phase 3 でエンジンから撤去され、ゲーム側のデータ駆動部品へ移る。

#include "ecs/ComponentMeta.h"
#include "ecs/Components.h"

#include <entt/entt.hpp>

namespace dx12e
{

void RegisterCoreComponentMeta()
{
    static bool done = false;
    if (done) return;
    done = true;

    // 値型 Vec3 (XMFLOAT3) を x/y/z で登録（Transform/Light 等の Vec3 フィールド用）。
    entt::meta_factory<DirectX::XMFLOAT3>{}
        .type("Vec3")
        .data<&DirectX::XMFLOAT3::x>("x")
        .data<&DirectX::XMFLOAT3::y>("y")
        .data<&DirectX::XMFLOAT3::z>("z");

    entt::meta_factory<NameTag>{}
        .type("NameTag")
        .data<&NameTag::name>("name");

    entt::meta_factory<Transform>{}
        .type("Transform")
        .data<&Transform::position>("position")
        .data<&Transform::rotation>("rotation")
        .data<&Transform::scale>("scale");

    entt::meta_factory<PointLight>{}
        .type("PointLight")
        .data<&PointLight::color>("color")
        .data<&PointLight::intensity>("intensity")
        .data<&PointLight::range>("range")
        .data<&PointLight::castShadows>("castShadows");

    entt::meta_factory<DirectionalLight>{}
        .type("DirectionalLight")
        .data<&DirectionalLight::direction>("direction")
        .data<&DirectionalLight::color>("color")
        .data<&DirectionalLight::intensity>("intensity")
        .data<&DirectionalLight::ambient>("ambient");

    entt::meta_factory<SpotLight>{}
        .type("SpotLight")
        .data<&SpotLight::color>("color")
        .data<&SpotLight::intensity>("intensity")
        .data<&SpotLight::range>("range")
        .data<&SpotLight::direction>("direction")
        .data<&SpotLight::innerConeDeg>("innerConeDeg")
        .data<&SpotLight::outerConeDeg>("outerConeDeg")
        .data<&SpotLight::castShadows>("castShadows");

    entt::meta_factory<CameraComponent>{}
        .type("CameraComponent")
        .data<&CameraComponent::fovDegrees>("fovDegrees")
        .data<&CameraComponent::nearClip>("nearClip")
        .data<&CameraComponent::farClip>("farClip")
        .data<&CameraComponent::isActive>("isActive")
        .data<&CameraComponent::projection>("projection")
        .data<&CameraComponent::orthoSize>("orthoSize");

    entt::meta_factory<RigidBody>{}
        .type("RigidBody")
        .data<&RigidBody::motionType>("motionType")
        .data<&RigidBody::mass>("mass")
        .data<&RigidBody::restitution>("restitution")
        .data<&RigidBody::friction>("friction")
        .data<&RigidBody::linearDamping>("linearDamping")
        .data<&RigidBody::angularDamping>("angularDamping")
        .data<&RigidBody::useGravity>("useGravity");

    entt::meta_factory<BoxCollider>{}
        .type("BoxCollider")
        .data<&BoxCollider::halfExtents>("halfExtents")
        .data<&BoxCollider::offset>("offset");

    entt::meta_factory<SphereCollider>{}
        .type("SphereCollider")
        .data<&SphereCollider::radius>("radius")
        .data<&SphereCollider::offset>("offset");

    entt::meta_factory<CapsuleCollider>{}
        .type("CapsuleCollider")
        .data<&CapsuleCollider::radius>("radius")
        .data<&CapsuleCollider::halfHeight>("halfHeight")
        .data<&CapsuleCollider::offset>("offset");

    entt::meta_factory<CharacterController>{}
        .type("CharacterController")
        .data<&CharacterController::radius>("radius")
        .data<&CharacterController::halfHeight>("halfHeight")
        .data<&CharacterController::offset>("offset")
        .data<&CharacterController::mass>("mass")
        .data<&CharacterController::maxSlopeDeg>("maxSlopeDeg")
        .data<&CharacterController::stepHeight>("stepHeight")
        .data<&CharacterController::jumpSpeed>("jumpSpeed")
        .data<&CharacterController::gravityScale>("gravityScale");

    entt::meta_factory<AudioSource>{}
        .type("AudioSource")
        .data<&AudioSource::clipPath>("clipPath")
        .data<&AudioSource::volume>("volume")
        .data<&AudioSource::loop>("loop")
        .data<&AudioSource::spatial>("spatial")
        .data<&AudioSource::playOnStart>("playOnStart")
        .data<&AudioSource::minDistance>("minDistance")
        .data<&AudioSource::maxDistance>("maxDistance");

    entt::meta_factory<Tag>{}
        .type("Tag")
        .data<&Tag::tags>("tags");

    entt::meta_factory<Sprite2D>{}
        .type("Sprite2D")
        .data<&Sprite2D::texturePath>("texturePath")
        .data<&Sprite2D::layer>("layer")
        .data<&Sprite2D::size>("size")
        .data<&Sprite2D::uvMin>("uvMin")
        .data<&Sprite2D::uvMax>("uvMax")
        .data<&Sprite2D::color>("color")
        .data<&Sprite2D::worldSpace>("worldSpace")
        .data<&Sprite2D::billboard>("billboard");

    entt::meta_factory<TrailRenderer>{}
        .type("TrailRenderer")
        .data<&TrailRenderer::emitting>("emitting")
        .data<&TrailRenderer::width>("width")
        .data<&TrailRenderer::life>("life")
        .data<&TrailRenderer::color>("color")
        .data<&TrailRenderer::colorEnd>("colorEnd")
        .data<&TrailRenderer::intensity>("intensity")
        .data<&TrailRenderer::blend>("blend")
        .data<&TrailRenderer::minDist>("minDist");

    entt::meta_factory<NetworkIdentity>{}
        .type("NetworkIdentity")
        .data<&NetworkIdentity::interestRadius>("interestRadius")
        .data<&NetworkIdentity::serverAuthority>("serverAuthority");

    entt::meta_factory<NetworkTransform>{}
        .type("NetworkTransform")
        .data<&NetworkTransform::syncMode>("syncMode")
        .data<&NetworkTransform::sendRate>("sendRate")
        .data<&NetworkTransform::syncPosition>("syncPosition")
        .data<&NetworkTransform::syncRotation>("syncRotation")
        .data<&NetworkTransform::syncScale>("syncScale")
        .data<&NetworkTransform::interpDelayMs>("interpDelayMs")
        .data<&NetworkTransform::snapDistance>("snapDistance");
}

} // namespace dx12e
