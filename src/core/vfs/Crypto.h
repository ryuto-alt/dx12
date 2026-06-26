#pragma once
//
// AES-256-GCM（Windows CNG / BCrypt）による暗号化・復号 + マスターキー組み立て。
// AEAD（機密性 + 完全性）を 1 パスで提供する。link: bcrypt.lib。
//
#include <cstdint>
#include <vector>
#include <array>
#include <cstddef>

namespace dx12e::vfs
{

inline constexpr std::size_t kKeyLen   = 32;
inline constexpr std::size_t kNonceLen = 12;
inline constexpr std::size_t kTagLen   = 16;

// generated/AssetKey.h の 4 フラグメントを XOR で復元し、32 バイトのマスターキーを
// out へ書き込む。呼び出し側は使用後に SecureZeroMemory(out) すること。
void AssembleKey(std::array<uint8_t, kKeyLen>& out);

// plain -> cipher（同じ長さ）。ランダムなノンスを生成し tag を埋める。
// CNG が失敗した場合は false。
bool AesGcmEncrypt(const uint8_t* key, const uint8_t* plain, std::size_t len,
                   uint8_t outNonce[kNonceLen], uint8_t outTag[kTagLen],
                   std::vector<uint8_t>& outCipher);

// cipher -> plain（同じ長さ）。tag を検証し、改竄（STATUS_AUTH_TAG_MISMATCH）や
// その他 CNG 失敗で false を返す。
bool AesGcmDecrypt(const uint8_t* key, const uint8_t* cipher, std::size_t len,
                   const uint8_t nonce[kNonceLen], const uint8_t tag[kTagLen],
                   std::vector<uint8_t>& outPlain);

} // namespace dx12e::vfs
