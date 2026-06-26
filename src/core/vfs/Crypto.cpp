#include "core/vfs/Crypto.h"
#include "core/generated/AssetKey.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace dx12e::vfs
{

void AssembleKey(std::array<uint8_t, kKeyLen>& out)
{
    using namespace detail;
    // フラグメント ^ マスク でキー 64bit を復元（リトルエンディアンで連結）。
    uint64_t k[4];
    k[0] = kFrag0 ^ kMask0;
    k[1] = kFrag1 ^ kMask1;
    k[2] = kFrag2 ^ kMask2;
    k[3] = kFrag3 ^ kMask3;
    std::memcpy(out.data(), k, kKeyLen);
    SecureZeroMemory(k, sizeof(k));
}

bool AesGcmEncrypt(const uint8_t* key, const uint8_t* plain, std::size_t len,
                   uint8_t outNonce[kNonceLen], uint8_t outTag[kTagLen],
                   std::vector<uint8_t>& outCipher)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<UCHAR> keyObj; // hKey が破棄されるまで生存させる

    auto run = [&]() -> bool {
        NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (st != 0)
            return false;
        st = BCryptSetProperty(
            hAlg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        if (st != 0)
            return false;

        DWORD cbKeyObj = 0;
        DWORD cbResult = 0;
        st = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&cbKeyObj),
                               sizeof(cbKeyObj), &cbResult, 0);
        if (st != 0)
            return false;
        keyObj.resize(cbKeyObj);

        st = BCryptGenerateSymmetricKey(hAlg, &hKey, keyObj.data(), cbKeyObj,
                                        const_cast<PUCHAR>(key),
                                        static_cast<ULONG>(kKeyLen), 0);
        if (st != 0)
            return false;

        st = BCryptGenRandom(nullptr, outNonce, static_cast<ULONG>(kNonceLen),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (st != 0)
            return false;

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = outNonce;
        info.cbNonce = static_cast<ULONG>(kNonceLen);
        info.pbTag   = outTag;
        info.cbTag   = static_cast<ULONG>(kTagLen);

        outCipher.resize(len);
        ULONG written = 0;
        st = BCryptEncrypt(hKey, const_cast<PUCHAR>(plain), static_cast<ULONG>(len),
                           &info, nullptr /*pIV=NULL for GCM*/, 0,
                           outCipher.empty() ? nullptr : outCipher.data(),
                           static_cast<ULONG>(outCipher.size()), &written, 0);
        if (st != 0)
            return false;
        outCipher.resize(written);
        return true;
    };

    const bool ok = run();
    if (hKey)
        BCryptDestroyKey(hKey);
    if (hAlg)
        BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool AesGcmDecrypt(const uint8_t* key, const uint8_t* cipher, std::size_t len,
                   const uint8_t nonce[kNonceLen], const uint8_t tag[kTagLen],
                   std::vector<uint8_t>& outPlain)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<UCHAR> keyObj;

    auto run = [&]() -> bool {
        NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (st != 0)
            return false;
        st = BCryptSetProperty(
            hAlg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        if (st != 0)
            return false;

        DWORD cbKeyObj = 0;
        DWORD cbResult = 0;
        st = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&cbKeyObj),
                               sizeof(cbKeyObj), &cbResult, 0);
        if (st != 0)
            return false;
        keyObj.resize(cbKeyObj);

        st = BCryptGenerateSymmetricKey(hAlg, &hKey, keyObj.data(), cbKeyObj,
                                        const_cast<PUCHAR>(key),
                                        static_cast<ULONG>(kKeyLen), 0);
        if (st != 0)
            return false;

        // 復号前にノンス + タグを info へ埋める。
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = static_cast<ULONG>(kNonceLen);
        info.pbTag   = const_cast<PUCHAR>(tag);
        info.cbTag   = static_cast<ULONG>(kTagLen);

        outPlain.resize(len);
        ULONG written = 0;
        st = BCryptDecrypt(hKey, const_cast<PUCHAR>(cipher), static_cast<ULONG>(len),
                           &info, nullptr /*pIV=NULL for GCM*/, 0,
                           outPlain.empty() ? nullptr : outPlain.data(),
                           static_cast<ULONG>(outPlain.size()), &written, 0);
        if (st != 0) // STATUS_AUTH_TAG_MISMATCH(0xC000A002) を含め、非ゼロは全て失敗
            return false;
        outPlain.resize(written);
        return true;
    };

    const bool ok = run();
    if (hKey)
        BCryptDestroyKey(hKey);
    if (hAlg)
        BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!ok)
        outPlain.clear();
    return ok;
}

} // namespace dx12e::vfs
