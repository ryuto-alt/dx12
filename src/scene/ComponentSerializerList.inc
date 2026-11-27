// コンポーネントのシリアライザの登録表。各レーンは自分のブロックの中にだけ足す（ブロックの外は書き換えない）。
//   include する側（ComponentSerializerRegistry.cpp、W05-15）が UNO_COMPONENT_SERIALIZER(Fn) を定義する。
//   ecs/Components.h にはアンカーを置かない（135 TU の再ビルドを避けるため）。
// ==== lane-A begin ====
// ==== lane-A end ====
// ==== lane-B begin ====
UNO_COMPONENT_SERIALIZER(RegisterVfxComponentSerializer)   // W1-BD4: UnoFX（"vfx"）
// ==== lane-B end ====
// ==== lane-C begin ====
UNO_COMPONENT_SERIALIZER(RegisterSphereSubdivSerializer)   // W1-C13: 球の分割数（"sphereSubdiv"）
// ==== lane-C end ====
// ==== lane-D begin ====
// ==== lane-D end ====
