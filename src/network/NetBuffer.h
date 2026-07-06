#pragma once
#include "core/Types.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace dx12e {

// バイナリ書き込みバッファ。全ターゲット Windows(x64/little-endian) 前提でバイトスワップ無し。
// ヘッダオンリー・GPU/entt 非依存の純ロジック(単体テスト対象)。
class NetWriter
{
public:
    void WriteU8(u8 v)   { Append(&v, sizeof(v)); }
    void WriteU16(u16 v) { Append(&v, sizeof(v)); }
    void WriteU32(u32 v) { Append(&v, sizeof(v)); }
    void WriteU64(u64 v) { Append(&v, sizeof(v)); }
    void WriteI32(i32 v) { Append(&v, sizeof(v)); }
    void WriteF32(f32 v) { Append(&v, sizeof(v)); }
    void WriteF64(f64 v) { Append(&v, sizeof(v)); }
    void WriteBool(bool v) { WriteU8(v ? 1 : 0); }

    void WriteString(const std::string& s)
    {
        WriteU32(static_cast<u32>(s.size()));
        Append(s.data(), s.size());
    }

    void WriteBytes(const void* data, size_t size) { Append(data, size); }

    const std::vector<u8>& Data() const { return m_data; }
    size_t Size() const { return m_data.size(); }

private:
    void Append(const void* src, size_t size)
    {
        const u8* p = static_cast<const u8*>(src);
        m_data.insert(m_data.end(), p, p + size);
    }

    std::vector<u8> m_data;
};

// 範囲外読み出し(壊れた/悪意あるパケット)は例外を投げる。呼び出し側(NetworkSystem::Poll)は
// try/catch で握り潰し、当該ピアを切断する想定(MCPブリッジのハンドラ例外握り潰しと同じ流儀)。
class NetReadError : public std::runtime_error
{
public:
    NetReadError() : std::runtime_error("NetReader: read past end of buffer") {}
};

class NetReader
{
public:
    NetReader(const u8* data, size_t size) : m_data(data), m_size(size) {}
    NetReader(const std::vector<u8>& data) : m_data(data.data()), m_size(data.size()) {}

    u8   ReadU8()   { return ReadRaw<u8>(); }
    u16  ReadU16()  { return ReadRaw<u16>(); }
    u32  ReadU32()  { return ReadRaw<u32>(); }
    u64  ReadU64()  { return ReadRaw<u64>(); }
    i32  ReadI32()  { return ReadRaw<i32>(); }
    f32  ReadF32()  { return ReadRaw<f32>(); }
    f64  ReadF64()  { return ReadRaw<f64>(); }
    bool ReadBool() { return ReadU8() != 0; }

    std::string ReadString()
    {
        u32 len = ReadU32();
        if (m_pos + len > m_size) throw NetReadError();
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return s;
    }

    void ReadBytes(void* dst, size_t size)
    {
        if (m_pos + size > m_size) throw NetReadError();
        std::memcpy(dst, m_data + m_pos, size);
        m_pos += size;
    }

    size_t Remaining() const { return m_size - m_pos; }
    size_t Position() const { return m_pos; }

private:
    template <typename T>
    T ReadRaw()
    {
        if (m_pos + sizeof(T) > m_size) throw NetReadError();
        T v;
        std::memcpy(&v, m_data + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return v;
    }

    const u8* m_data;
    size_t m_size;
    size_t m_pos = 0;
};

} // namespace dx12e
