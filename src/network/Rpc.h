#pragma once
#include "core/Types.h"
#include "network/NetBuffer.h"

#include <DirectXMath.h>
#include <string>
#include <vector>

namespace dx12e {

// Lua net:rpc の引数1個分。number/string/bool/vec3のみ対応(sol2側で変換)。
// テーブルや関数等の複雑な値は渡せない(RPCは軽量なイベント通知用途を想定)。
struct RpcValue
{
    enum class Type : u8 { Nil = 0, Bool = 1, Number = 2, String = 3, Vec3 = 4 };

    Type type = Type::Nil;
    bool b = false;
    f64  num = 0.0;
    std::string str;
    DirectX::XMFLOAT3 vec{ 0.0f, 0.0f, 0.0f };

    static RpcValue MakeBool(bool v)                    { RpcValue r; r.type = Type::Bool;   r.b = v;             return r; }
    static RpcValue MakeNumber(f64 v)                   { RpcValue r; r.type = Type::Number; r.num = v;           return r; }
    static RpcValue MakeString(std::string v)           { RpcValue r; r.type = Type::String; r.str = std::move(v); return r; }
    static RpcValue MakeVec3(const DirectX::XMFLOAT3& v){ RpcValue r; r.type = Type::Vec3;   r.vec = v;           return r; }
};
using RpcArgs = std::vector<RpcValue>;

inline void WriteRpcArgs(NetWriter& w, const RpcArgs& args)
{
    w.WriteU32(static_cast<u32>(args.size()));
    for (const auto& a : args)
    {
        w.WriteU8(static_cast<u8>(a.type));
        switch (a.type)
        {
        case RpcValue::Type::Nil:    break;
        case RpcValue::Type::Bool:   w.WriteBool(a.b); break;
        case RpcValue::Type::Number: w.WriteF64(a.num); break;
        case RpcValue::Type::String: w.WriteString(a.str); break;
        case RpcValue::Type::Vec3:
            w.WriteF32(a.vec.x); w.WriteF32(a.vec.y); w.WriteF32(a.vec.z);
            break;
        }
    }
}

inline RpcArgs ReadRpcArgs(NetReader& r)
{
    RpcArgs args;
    const u32 count = r.ReadU32();
    args.reserve(count);
    for (u32 i = 0; i < count; ++i)
    {
        const auto type = static_cast<RpcValue::Type>(r.ReadU8());
        RpcValue v; v.type = type;
        switch (type)
        {
        case RpcValue::Type::Nil:    break;
        case RpcValue::Type::Bool:   v.b = r.ReadBool(); break;
        case RpcValue::Type::Number: v.num = r.ReadF64(); break;
        case RpcValue::Type::String: v.str = r.ReadString(); break;
        case RpcValue::Type::Vec3:
            v.vec.x = r.ReadF32(); v.vec.y = r.ReadF32(); v.vec.z = r.ReadF32();
            break;
        }
        args.push_back(std::move(v));
    }
    return args;
}

} // namespace dx12e
