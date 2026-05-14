-- player_controller.lua
-- WASD で移動 + Space でジャンプするキャラクターコントローラー。
-- モデル Entity に LuaScript としてアタッチして使う。
--
-- 前提条件 (Inspector で設定):
--   - RigidBody: MotionType = Dynamic, Mass = 適当 (5〜60)
--   - Collider:  Capsule / Box / ConvexHull のいずれか1つ

local moveSpeed       = 5.0
local jumpImpulse     = 7.0
local groundCheckDist = 1.2

function OnStart(self)
    log("ready: " .. (self.name or "?"))
    if not self.this:isValid() then
        log("  WARN self.this invalid")
        return
    end
    -- 必要コンポーネントが揃っているか診断
    local hasRb = self.this:hasComponent("RigidBody")
    local hasCol = self.this:hasComponent("BoxCollider")
                or self.this:hasComponent("SphereCollider")
                or self.this:hasComponent("CapsuleCollider")
                or self.this:hasComponent("ConvexHullCollider")
    log("  RigidBody=" .. tostring(hasRb) .. " Collider=" .. tostring(hasCol))
    if not hasRb then
        log("  -> Inspector で RigidBody(Dynamic) を追加してや")
    end
    if not hasCol then
        log("  -> Inspector で Collider(Box/Capsule等) を追加してや")
    end
end

function OnUpdate(self, dt)
    local me = self.this
    if not me:isValid() then return end

    -- カメラ Yaw から「前方/右方向」の XZ
    local yaw = math.rad(camera:getYaw())
    local fx, fz = math.sin(yaw),  math.cos(yaw)
    local rx, rz = math.cos(yaw), -math.sin(yaw)

    -- WASD 入力 + 押下時にログ (Toolbar の [Lua] 表示で確認できる)
    local mx, mz = 0, 0
    if input:isAsyncKeyDown(KEY_W) then mx = mx + fx; mz = mz + fz; log("W") end
    if input:isAsyncKeyDown(KEY_S) then mx = mx - fx; mz = mz - fz; log("S") end
    if input:isAsyncKeyDown(KEY_D) then mx = mx + rx; mz = mz + rz; log("D") end
    if input:isAsyncKeyDown(KEY_A) then mx = mx - rx; mz = mz - rz; log("A") end

    local len = math.sqrt(mx * mx + mz * mz)
    if len > 0.0001 then
        mx, mz = mx / len, mz / len
    end

    -- 縦速度 (重力 + ジャンプ) は維持し、水平のみ上書き
    local v = physics:getVelocity(me)
    physics:setVelocity(me, Vec3.new(mx * moveSpeed, v.y, mz * moveSpeed))

    -- 移動が反映されてるか速度ログ (動いてないか確認用、押下時のみ)
    if len > 0.0001 then
        local v2 = physics:getVelocity(me)
        log(string.format("vel=(%.2f,%.2f,%.2f) want_h=(%.2f,%.2f)",
            v2.x, v2.y, v2.z, mx * moveSpeed, mz * moveSpeed))
    end

    -- Space ジャンプ
    if input:isKeyPressed(KEY_SPACE) then
        local pos = self.transform.position
        local hit = physics:raycast(
            Vec3.new(pos.x, pos.y, pos.z),
            Vec3.new(0, -1, 0),
            groundCheckDist
        )
        log("Space hit=" .. tostring(hit.hit) .. " dist=" .. tostring(hit.distance))
        if hit.hit then
            physics:applyImpulse(me, Vec3.new(0, jumpImpulse, 0))
        end
    end
end
