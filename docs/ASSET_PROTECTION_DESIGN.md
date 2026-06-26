# Asset Protection & Game-Only Runtime Design

Status: design spec, ready to implement.
Author: architect (synthesized from web research + codebase load-point map).
Audience: an engineer with the repo open and no other context.

Repo root: `C:/Users/ryuto/Documents/GitHub/dx12`. C++20, `namespace dx12e`, `/W4 /WX`,
VS2022 + Ninja, vcpkg. Builds Debug AND Release. ONE exe today is both editor and game.

This document covers two deliverables that are implemented together:

- **(A) Game-only runtime (`GameRuntime` / shipped `Game.exe`)** — a build that is the game and
  can NEVER become the editor, even if the user deletes every config file next to the exe.
- **(B) Encrypted asset packaging (`game.pak`)** — audio, models, sprites, textures, Lua scripts,
  scene/prefab/sceneflow JSON packed into one encrypted archive at build time, transparently
  decrypted to memory at runtime through a VFS. The editor keeps reading loose files from disk and
  `--validate` keeps working unchanged.

---

## 0. Threat model & honest scope (read first)

Client-side asset encryption is a **deterrent, not a lock**. The key must be in process memory at
decrypt time, so a reverse engineer with x64dbg/IDA and a breakpoint on `BCryptDecrypt` recovers it
in under an hour from any build. The goal here is calibrated for a hobby/indie engine:

- Defeat `strings` / hex-dump / drag-into-AssetStudio casual ripping.
- Defeat automated mass-extraction scripts that memorize key offsets across builds (per-build random
  key + fragment splitting breaks these).
- Make tampering **detectable** (AES-256-GCM auth tag), so corrupted/modified assets fail to load
  instead of feeding crafted bytes to parsers.

We do NOT claim protection against a skilled, motivated reverse engineer. Do not over-invest.

The game-only runtime (A) is a **hard guarantee** (compile-time), not a deterrent: there is no
runtime branch that can flip `GameRuntime` into the editor.

---

## 1. Design decisions (decided — do not re-litigate)

| Topic | Decision | Why |
|---|---|---|
| Cipher | **AES-256-GCM via Windows CNG (BCrypt)** | AEAD (confidentiality+integrity in one pass), AES-NI 2-3 GB/s, FIPS in-box. Zero new dependency: link `bcrypt.lib` (Windows SDK). |
| Per-entry crypto | 12-byte random nonce + 16-byte GCM tag, **per entry**, stored in TOC | Random access; tamper scoped per file; nonce uniqueness trivial offline. |
| Compression | **Windows Compression API `COMPRESS_ALGORITHM_XPRESS_HUFF`** (cabinet.dll / `cabinet.lib`) | No new dependency. Compress-then-encrypt. Per-entry "store if not smaller". |
| Key storage | **Per-build random 32-byte key**, split into 4 XOR-masked fragments in generated header, assembled on the stack at decrypt time, `SecureZeroMemory`'d after | Best realistic client-side deterrence. |
| Pak layout | **Header-at-start, data section, TOC-at-end, string table last** (UE/ZIP philosophy) | Streamable writer; single seek to find TOC. |
| Path lookup | **FNV-1a 64-bit of normalized path** (lowercase, `/`, assets-relative) | O(1) in-memory lookup, no string compare in release. |
| Model external files | **Custom `Assimp::IOSystem` backed by the VFS** (not GLB pre-bake) | Existing assets ship split `.gltf`+`.bin` (`assets/models/human/walk.gltf/bin`); IOSystem handles `.gltf`/`.bin`/`.obj`/`.mtl`/external textures uniformly with one minimal change and no asset re-authoring. |
| Game-only guarantee | **`DX12_GAME_RUNTIME` compile define on a dedicated `GameRuntime` exe target**, applied to its own compile of `src/main.cpp` | `main.cpp` is compiled per-exe, so the define forces game mode at compile time. No runtime branch reads config for the mode decision. |
| Editor code in game exe | **Stays linked (dead code), gated behind the existing `m_isGameMode` runtime checks** | Lowest risk under `/WX`: avoids compiling `Core` twice and avoids `#ifdef`-surgery across the 4700-line `Application.cpp`. The mode can never be false, so the editor is unreachable. (Optional later hardening in §11.) |
| Shaders (`.cso`) | **NOT encrypted, NOT in pak** — copied loose to `shaders/` as today | Engine-owned DXBC/DXIL, no game IP. Touching the shader path adds risk for no benefit. |
| Boot config | **Stored inside the pak** (`__manifest__` entry), no shippable `game.json` | Removes the deletable-config vulnerability entirely. |

---

## 2. New module layout

All new crypto/pak/vfs code goes into the **`Core`** static library (`src/core/`). Rationale:
every consumer lib (`Resource`, `Audio`, `Scene`, `Scripting`) already links `Core` **PUBLIC**, and
`Core` is the lowest node in the dependency graph (it owns `PathResolver`/`Logger`). No new CMake
subdir, no dependency cycle. The VFS belongs conceptually next to `PathResolver`.

New files (all under `src/core/`):

```
src/core/vfs/Crypto.h          // AES-256-GCM (BCrypt) encrypt/decrypt + key assembly
src/core/vfs/Crypto.cpp
src/core/vfs/Compression.h     // XPRESS_HUFF compress/decompress (cabinet)
src/core/vfs/Compression.cpp
src/core/vfs/PakFormat.h       // shared on-disk structs + FNV-1a + normalize() (header-only-ish)
src/core/vfs/PakWriter.h       // build-time packer (used by Application::BuildGame)
src/core/vfs/PakWriter.cpp
src/core/vfs/PakArchive.h      // runtime mounted archive (reader)
src/core/vfs/PakArchive.cpp
src/core/vfs/Vfs.h             // public facade: namespace dx12e::vfs
src/core/vfs/Vfs.cpp
src/core/vfs/VfsIOSystem.h     // Assimp::IOSystem/IOStream backed by the VFS
src/core/vfs/VfsIOSystem.cpp   // (links assimp -> lives in Resource, see §9.3 note)
src/core/generated/AssetKey.h  // GIT-IGNORED, generated per build (see §6)
tools/gen_asset_key.ps1        // key generator invoked by build_*.bat
```

Note on `VfsIOSystem`: it `#include`s assimp headers, which `Core` does not link. Put
`VfsIOSystem.h/.cpp` in the **`Resource`** library instead (`src/resource/VfsIOSystem.*`), since
`Resource` already links assimp. It depends only on `vfs::ReadAsset` from `Core`. Everything else
stays in `Core`.

---

## 3. Pak binary format (`game.pak`)

All integers little-endian (x86-64). On-disk structs wrapped in `#pragma pack(push,1)/pop`. Read
fields with `memcpy` into local PODs to avoid misaligned-access UB.

```
+--------------------+  offset 0
| PakHeader (32 B)   |
+--------------------+
| data section       |  concatenated per-entry blobs, each padded to 16-byte boundary.
|  entry0 ciphertext |  (ciphertext length == compressedSize; GCM does not expand plaintext)
|  pad..             |
|  entry1 ciphertext |
|  ...               |
+--------------------+  <- toc_offset
| TOC: PakEntry[N]   |  N == entry_count, 64 bytes each
+--------------------+
| string table       |  strtab_size bytes, null-terminated normalized paths (DEBUG/dev only;
|                    |  0 bytes in hardened release where stripStrings == true)
+--------------------+
| footer magic (4 B) |  repeated magic 'GMPK' = write-completed sentinel
+--------------------+  EOF
```

### 3.1 `PakHeader` (32 bytes)

```cpp
#pragma pack(push,1)
struct PakHeader {
    uint32_t magic;        // 'GMPK' = 0x4B504D47 (little-endian bytes 'G','M','P','K')
    uint32_t version;      // = 1
    uint32_t endianSentinel;// = 0x01020304 (detect byte-swapped load)
    uint32_t flags;        // bit0 any_xpress, bit1 entries_encrypted, bit2 strings_stripped
    uint64_t tocOffset;    // absolute offset of first PakEntry
    uint32_t entryCount;   // N
    uint32_t strtabSize;   // bytes of string table (0 if stripped)
};
#pragma pack(pop)
static_assert(sizeof(PakHeader) == 32);
```

### 3.2 `PakEntry` (64 bytes)

```cpp
#pragma pack(push,1)
struct PakEntry {
    uint64_t pathHash;     // FNV-1a 64 of normalized path
    uint64_t dataOffset;   // absolute offset of ciphertext in data section
    uint32_t storedSize;   // bytes on disk = ciphertext length = compressed length
    uint32_t originalSize; // plaintext (pre-compress) length
    uint16_t flags;        // bit0 compressed(xpress_huff), bit1 encrypted
    uint16_t nameOff_lo;   // low 16 bits of string-table offset (0xFFFF if stripped)... see note
    uint8_t  nonce[12];    // GCM nonce (random per entry)
    uint8_t  tag[16];      // GCM auth tag
    uint8_t  _reserved[6]; // zero; pads to 64
};
#pragma pack(pop)
static_assert(sizeof(PakEntry) == 64);
```

Note on `nameOff`: 16 bits is too small for a real string table. Use the 6 reserved bytes: define
`uint32_t nameOff` instead of `nameOff_lo` + 2 of the reserved bytes, leaving 4 reserved. Concretely
lay the tail out as: `uint8_t nonce[12]; uint8_t tag[16]; uint32_t nameOff; uint8_t _reserved[4];`
to keep exactly 64 bytes (8+8+4+4+2 + 12+16+4+4 = 62 ... recount below).

Field sizes recount (must total 64):
`pathHash 8 + dataOffset 8 + storedSize 4 + originalSize 4 + flags 2 + _pad0 2 + nonce 12 + tag 16 +
nameOff 4 + _reserved 4 = 64`. Use this exact layout:

```cpp
#pragma pack(push,1)
struct PakEntry {
    uint64_t pathHash;     // 0
    uint64_t dataOffset;   // 8
    uint32_t storedSize;   // 16
    uint32_t originalSize; // 20
    uint16_t flags;        // 24  bit0 compressed, bit1 encrypted
    uint16_t _pad0;        // 26
    uint8_t  nonce[12];    // 28
    uint8_t  tag[16];      // 40
    uint32_t nameOff;      // 56  byte offset into string table; 0xFFFFFFFF if stripped
    uint32_t _reserved;    // 60
};                          // 64
#pragma pack(pop)
static_assert(sizeof(PakEntry) == 64);
```

### 3.3 Special entries

Two reserved keys are stored as ordinary entries (compressed+encrypted like the rest):

- `"__manifest__"` — UTF-8 JSON `{ "title", "startScene", "windowWidth", "windowHeight" }`. Replaces
  shipped `game.json` for boot config.
- (optional) `"__filelist__"` — newline-joined normalized paths, only in dev paks; redundant with the
  string table, skip in v1.

### 3.4 Path normalization (THE one canonical function — shared by writer and runtime)

`PakFormat.h`:

```cpp
namespace dx12e::vfs {
// lowercase, backslash->slash, strip leading '/', strip a leading "assets/" prefix.
std::string Normalize(std::string_view path);
uint64_t    FnvHash(std::string_view normalized); // FNV-1a 64: basis 0xcbf29ce484222325, prime 0x100000001b3
}
```

Both `PakWriter` (compute hash at pack) and the runtime path->key conversion MUST call `Normalize`
then `FnvHash`. A mismatch = silent "asset not found". Keep the string table in dev builds and
`assert` on hash collision during packing.

---

## 4. Crypto module (`src/core/vfs/Crypto.{h,cpp}`)

AES-256-GCM via CNG. One algorithm provider opened once (GCM chaining set on the **algorithm
handle** before key creation), reused. Key handle re-created per file (CNG key handles are stateful;
do not reuse across `BCryptEncrypt`/`Decrypt` calls — re-import per call is cheapest and correct).

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <array>

namespace dx12e::vfs {

inline constexpr size_t kKeyLen   = 32;
inline constexpr size_t kNonceLen = 12;
inline constexpr size_t kTagLen   = 16;

// Returns the assembled 32-byte master key on the stack of the CALLER's array.
// Implemented in Crypto.cpp; reads the 4 fragments from generated/AssetKey.h, XOR-unmasks,
// writes into out. Caller MUST SecureZeroMemory(out) after use.
void AssembleKey(std::array<uint8_t, kKeyLen>& out);

// Encrypt plain -> cipher (same length). Generates a random nonce, fills tag.
// Returns false on any CNG failure.
bool AesGcmEncrypt(const uint8_t* key, const uint8_t* plain, size_t len,
                   uint8_t outNonce[kNonceLen], uint8_t outTag[kTagLen],
                   std::vector<uint8_t>& outCipher);

// Decrypt cipher -> plain (same length). Verifies tag; returns false on
// STATUS_AUTH_TAG_MISMATCH (tampered) or any CNG failure.
bool AesGcmDecrypt(const uint8_t* key, const uint8_t* cipher, size_t len,
                   const uint8_t nonce[kNonceLen], const uint8_t tag[kTagLen],
                   std::vector<uint8_t>& outPlain);
} // namespace dx12e::vfs
```

CNG sequence (encrypt), exact:

1. `BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0)`.
2. `BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PBYTE)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0)`
   — `sizeof` of the wide literal includes its NUL (`(wcslen+1)*2` bytes). Set on `hAlg` BEFORE key.
3. `BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, ...)` -> alloc `pbKeyObj` (keep alive until DestroyKey).
4. `BCryptGenerateSymmetricKey(hAlg, &hKey, pbKeyObj, cbKeyObj, key, 32, 0)`.
5. `BCryptGenRandom(NULL, nonce, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG)`.
6. `BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);`
   `info.pbNonce=nonce; info.cbNonce=12; info.pbTag=tag; info.cbTag=16;` (pbAuthData NULL, cbAuthData 0).
7. `BCryptEncrypt(hKey, plain, (ULONG)len, &info, NULL/*pIV*/, 0, cipher, (ULONG)len, &written, 0)`
   — **pIV MUST be NULL** for GCM; flags 0 (no `BCRYPT_BLOCK_PADDING`).
8. Tag is now in `info.pbTag`. `BCryptDestroyKey`; free `pbKeyObj`; `BCryptCloseAlgorithmProvider`.

Decrypt is identical except step 5 is skipped, the stored nonce+tag are placed in `info` BEFORE the
call, and `BCryptDecrypt` returns `STATUS_AUTH_TAG_MISMATCH` (0xC000A002) on tamper — treat any
nonzero `NTSTATUS` as failure and return false (caller aborts the load).

Pitfalls to honor (from research): nonce exactly 12 B; chaining mode set on alg handle pre-key;
pIV NULL; pbKeyObj outlives the key; pre-fill tag before decrypt; flags 0.

---

## 5. Compression module (`src/core/vfs/Compression.{h,cpp}`)

Windows Compression API, buffer mode (no `COMPRESS_RAW`), `COMPRESS_ALGORITHM_XPRESS_HUFF`. Link
`cabinet.lib`. Store the original size in the TOC, never rely on runtime size queries.

```cpp
namespace dx12e::vfs {
// Compress src; on success outCompressed holds the result. Returns true ONLY if the result is
// strictly smaller than src (else caller stores uncompressed and clears the compressed flag).
bool XpressCompress(const uint8_t* src, size_t len, std::vector<uint8_t>& outCompressed);

// Decompress src (XPRESS_HUFF) into outPlain sized to expectedOriginalSize (from the TOC).
bool XpressDecompress(const uint8_t* src, size_t len, size_t expectedOriginalSize,
                      std::vector<uint8_t>& outPlain);
}
```

Compress decision at pack time: try compress; if `!XpressCompress(...)` or result >= original*0.95,
store uncompressed and leave `flags.compressed = 0`. Already-compressed inputs (`.png`, `.jpg`,
`.mp3`, `.ogg`, `.dds`) almost always fail the threshold -> stored. `.lua`, `.json`, `.gltf`,
`.bin`, `.wav`, `.obj` compress well.

Pipeline order (mandatory): **compress THEN encrypt** (encrypted data is high-entropy and will not
compress). Decrypt THEN decompress at runtime.

---

## 6. Key obfuscation (`generated/AssetKey.h` + `tools/gen_asset_key.ps1`)

- `tools/gen_asset_key.ps1` generates a random 32-byte key, splits into 4 × 8-byte fragments, picks 4
  random 8-byte XOR masks, and writes `src/core/generated/AssetKey.h`:

```cpp
#pragma once
#include <cstdint>
// AUTO-GENERATED — DO NOT COMMIT. Regenerated only if absent (stable across Debug/Release).
namespace dx12e::vfs::detail {
inline constexpr uint64_t kMask0 = 0x....ULL; inline constexpr uint64_t kFrag0 = 0x....ULL; // frag = key^mask
inline constexpr uint64_t kMask1 = 0x....ULL; inline constexpr uint64_t kFrag1 = 0x....ULL;
inline constexpr uint64_t kMask2 = 0x....ULL; inline constexpr uint64_t kFrag2 = 0x....ULL;
inline constexpr uint64_t kMask3 = 0x....ULL; inline constexpr uint64_t kFrag3 = 0x....ULL;
}
```

- `Crypto.cpp::AssembleKey` reconstructs `key64[i] = kFragI ^ kMaskI` into a stack array, never a
  global. Keep the four `uint64_t` XORs in `AssembleKey` only; rely on `/O2 /GL` LTCG to inline.
- `.gitignore`: add `src/core/generated/`.
- `build_debug.bat` / `build_release.bat`: before the cmake build, run
  `powershell -ExecutionPolicy Bypass -File tools/gen_asset_key.ps1` which **no-ops if the header
  already exists**. This guarantees Debug and Release (and the editor packer + GameRuntime) share one
  key, while each fresh clone/clean gets a unique key.
- CMake fallback: if `src/core/generated/AssetKey.h` is missing at configure time, emit a
  `FATAL_ERROR` telling the dev to run the generator (or add a `add_custom_command` that runs the ps1
  at build). Decision: run it from the .bat (simplest, already the entry point); add a CMake
  existence check that fails fast with a clear message.

Honest note: this stops automated/static extraction and per-version cracking, not a debugger.

---

## 7. VFS facade (`src/core/vfs/Vfs.{h,cpp}`)

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dx12e::vfs {

// Mount a pak (GameRuntime boot). Returns false if file missing/invalid -> caller may run disk mode.
bool MountPak(const std::string& pakPath);
void Unmount();

// true once a pak is mounted (== shipped game). false in editor / --validate (disk mode).
bool InGameMode();

// Read an asset by ASSETS-RELATIVE path (e.g. "textures/foo.png", "scenes/title.json").
// Game mode: decrypt+decompress from the mounted pak. Disk mode: read loose file from AssetsDir.
// Returns empty vector on miss/failure.
std::vector<uint8_t> ReadAsset(const std::string& relPath);

// Convenience: accept an ABSOLUTE path (as loaders build via AssetsDir()+rel), strip the AssetsDir
// prefix to recover the relative key, then ReadAsset. wide overload included for texture callers.
std::vector<uint8_t> ReadAssetAbs(const std::string& absPath);
std::vector<uint8_t> ReadAssetAbs(const std::wstring& absPath);

bool Exists(const std::string& relPath);

// Boot config read from the pak "__manifest__" entry (game mode). Empty fields in disk mode.
struct BootConfig { std::string title; std::string startScene; int windowWidth=1280, windowHeight=720; };
bool ReadBootConfig(BootConfig& out);
} // namespace dx12e::vfs
```

`ReadAssetAbs` recovers the key by stripping `PathResolver::AssetsDir()` (normalized, both `\\`->`/`,
lowercase compare) from the front; if the prefix is absent (e.g. AssetBrowser absolute paths outside
assets/, or `C:\\Windows\\Fonts`), it returns empty in game mode and (disk mode) falls back to a raw
file read of the absolute path. This preserves editor behavior.

Internals: `Vfs.cpp` owns a single `std::optional<PakArchive> g_pak`. `InGameMode()` == `g_pak`
mounted. Disk-mode `ReadAsset` does `std::ifstream(AssetsDir()/relPath, binary)` -> bytes.

---

## 8. PakWriter & PakArchive

### 8.1 `PakWriter` (build-time, runs inside the editor exe during `BuildGame`)

```cpp
namespace dx12e::vfs {
class PakWriter {
public:
    bool Open(const std::string& outPath);              // writes 32 zero bytes placeholder header
    // Add one file: read srcAbs, compress(maybe), encrypt, append padded blob, record TOC entry
    // keyed by Normalize(relPath). Skips zero-byte files with a warning.
    bool AddFile(const std::string& srcAbs, const std::string& relPath);
    bool AddBlob(const std::string& relPath, const uint8_t* data, size_t len); // for __manifest__
    bool Finish(bool stripStrings);                      // write TOC, strtab, footer, patch header
private:
    /* HANDLE/ofstream, std::vector<PakEntry>, string table buffer, current data offset */
};
}
```

Algorithm (Finish): after all `AddFile`, the file pointer is at end of data section -> record
`tocOffset`; write `entryCount` × `PakEntry`; write string table (unless stripStrings); write 4-byte
footer magic; seek to 0 and write the real `PakHeader`. Use an **atomic write**: write to
`game.pak.tmp`, then `rename` over `game.pak` so an interrupted build never ships a half-written pak.

`AddFile` per-entry steps: read bytes; `XpressCompress` (set compressed flag iff smaller);
`AssembleKey` once per writer (cache assembled key in the writer; zero it in `Finish`);
`AesGcmEncrypt` -> nonce/tag/cipher; pad current offset to 16; write cipher; fill `PakEntry`.

### 8.2 `PakArchive` (runtime reader)

```cpp
namespace dx12e::vfs {
class PakArchive {
public:
    bool Mount(const std::string& path);  // open, validate header+footer, load TOC + lookup map
    bool Read(const std::string& relPath, std::vector<uint8_t>& out) const; // normalize->hash->read
    bool Has(const std::string& relPath) const;
    ~PakArchive();                         // CloseHandle (RAII; Mount idempotent: re-mount closes old)
private:
    /* HANDLE hFile (CreateFileW GENERIC_READ|SHARE_READ|OPEN_EXISTING|FILE_FLAG_RANDOM_ACCESS),
       std::vector<PakEntry> toc_, std::unordered_map<uint64_t,uint32_t> lookup_,
       std::array<uint8_t,32> key_  (assembled at Mount, zeroed at dtor) */
};
}
```

`Read`: `Normalize`->`FnvHash`->`lookup_`; on hit `SetFilePointerEx(dataOffset)` + `ReadFile(storedSize)`;
`AesGcmDecrypt` (abort on tag mismatch); if compressed `XpressDecompress(originalSize)`. Return plain.
TOC + lookup map are fully resident in RAM after `Mount` (zero I/O per lookup).

Mount validates: `magic=='GMPK'`, `version==1`, `endianSentinel==0x01020304`, last-4-bytes footer
magic present (write-completed). On any failure return false (GameRuntime then shows a fatal error;
do not silently fall to disk in game mode).

---

## 9. Loader hooks (exact edits)

The pattern everywhere: at the existing read chokepoint, call `vfs::ReadAsset(rel)` (or
`ReadAssetAbs`). If non-empty, feed the existing `*FromMemory` API. Disk mode also returns bytes
(loose read), so the memory path is the single hot path; the only true fallback is "file missing".

### 9.1 Textures — primary chokepoint `ResourceManager::GetOrLoadTexture`
File: `src/resource/ResourceManager.cpp:90`.

Inside, before the cache-miss `TextureLoader::LoadFromFile` call: compute formatHint from extension
(`L".dds"` -> `"dds"` else `""`), then
`auto bytes = vfs::ReadAssetAbs(filePath);` and if `!bytes.empty()` call
`TextureLoader::LoadFromMemory(*m_device, cmdList, bytes.data(), bytes.size(), formatHint, srgb)`.
Keep the **cache key = the original absolute `filePath` wstring** so cache hit/miss logic is
unchanged. Fall through to `LoadFromFile` only if `bytes.empty()` (disk mode, file truly absent).

This single hook covers: editor icons (`Application.cpp:1522`), world/HUD `Sprite2D`
(`Application.cpp:2884`, `4142`), Lua `ui:image` (`Application.cpp:4121`), AssetBrowser thumbnails
(`AssetBrowserPanel.cpp:102`), model albedo (`ModelLoader.cpp:758`), model PBR
(`ModelLoader.cpp:791`). No per-call-site edits needed.

### 9.2 Skybox cubemap — secondary chokepoint (bypasses ResourceManager)
File: `src/core/Application.cpp:1628` calls `TextureLoader::LoadCubeFromFile` directly.

Add a new memory API to `TextureLoader` (`src/resource/TextureLoader.h:30`, impl after line 175):

```cpp
static std::unique_ptr<Texture> LoadCubeFromMemory(
    GraphicsDevice& device, ID3D12GraphicsCommandList* cmdList,
    const uint8_t* data, size_t dataSize, bool srgb = false);
```

Impl mirrors `LoadCubeFromFile` (lines 93-175) but starts with
`DirectX::LoadFromDDSMemory(data, dataSize, DDS_FLAGS_NONE, nullptr, scratch)` and validates
`metadata.IsCubemap() && metadata.arraySize == 6`. At `Application.cpp:1628`:

```cpp
auto bytes = vfs::ReadAsset(sky.envMapPath); // sky.envMapPath is assets-relative
m_envCubeTex = bytes.empty()
    ? TextureLoader::LoadCubeFromFile(*m_graphicsDevice, cmd, wpath, false)   // disk fallback
    : TextureLoader::LoadCubeFromMemory(*m_graphicsDevice, cmd, bytes.data(), bytes.size(), false);
```

### 9.3 Models — `ModelLoader::LoadFromFile` + custom Assimp IOSystem
Files: `src/resource/ModelLoader.cpp:456` (`ReadFile`) and `:869` (`LoadAnimationsFromFile` `ReadFile`).

Do NOT switch to `ReadFileFromMemory`. Instead install a VFS-backed `Assimp::IOSystem` so the
**unchanged** `importer.ReadFile(filePath, flags)` transparently resolves the `.gltf`, its `.bin`,
its `.mtl`, and external textures through the VFS:

```cpp
// src/resource/VfsIOSystem.h  (lives in Resource lib; links assimp)
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>
namespace dx12e {
class VfsIOSystem : public Assimp::IOSystem {
public:
    bool Exists(const char* file) const override;          // vfs::ReadAssetAbs(file) non-empty || fs::exists
    char getOsSeparator() const override { return '/'; }
    Assimp::IOStream* Open(const char* file, const char* mode = "rb") override; // returns VfsIOStream(bytes)
    void Close(Assimp::IOStream* s) override { delete s; }
};
} // VfsIOStream wraps a std::vector<uint8_t> with Read/Tell/FileSize/Seek; Write is a no-op (read-only).
```

In `ModelLoader::LoadFromFile` (and `LoadAnimationsFromFile`), before `ReadFile`:

```cpp
Assimp::Importer importer;
importer.SetIOHandler(new VfsIOSystem()); // importer takes ownership (deletes on its own dtor)
// ...existing flags...
const aiScene* scene = importer.ReadFile(filePath.string(), flags); // UNCHANGED call
```

`VfsIOSystem::Open` computes the key via `vfs::ReadAssetAbs(file)`; assimp passes both the primary
path and sibling relative paths (resolved by assimp against the primary's dir) — all become
AssetsDir-relative keys after `Normalize`. In disk mode `vfs::ReadAssetAbs` returns the loose bytes,
so the editor path is identical (no need to swap back to the default IOSystem). Embedded-texture
codepaths (`ModelLoader.cpp:721, 742, 783` -> `GetOrLoadEmbeddedTexture`) need **no change** — bytes
already come from the in-memory `aiScene`.

This is the single highest-complexity change; budget time for it and round-trip a split `.gltf`+`.bin`
through a pak first.

### 9.4 Audio — `AudioSystem::GetOrLoadClip`
File: `src/audio/AudioSystem.cpp:144`. Add to `AudioClip` (`src/audio/AudioClip.h:15`):

```cpp
bool LoadFromMemory(const uint8_t* data, size_t size, const std::string& extHint); // dispatch wav/mp3
private:
bool LoadWavFromMemory(const uint8_t* data, size_t size);  // drwav_init_memory
bool LoadMp3FromMemory(const uint8_t* data, size_t size);  // drmp3_open_memory_and_read_pcm_frames_s16
```

`LoadWavFromMemory`/`LoadMp3FromMemory` are line-for-line copies of `LoadWav` (`AudioClip.cpp:36`) /
`LoadMp3` (`:75`) with `drwav_init_file`->`drwav_init_memory(&wav, data, size, NULL)` and
`drmp3_open_file_and_read_pcm_frames_s16(path,...)`->`drmp3_open_memory_and_read_pcm_frames_s16(data,size,...)`.

In `GetOrLoadClip`, after building `fullPath` and on cache miss:
`auto bytes = vfs::ReadAsset(filePath);` (filePath is already assets-relative — the cache key); if
non-empty `clip->LoadFromMemory(bytes.data(), bytes.size(), extFromPath)` else
`clip->LoadFromFile(fullPath)`. No change to `PlayBGM/PlaySFX/PlaySFXSpatial`.

### 9.5 Lua — `ScriptEngine`
Files: `src/scripting/ScriptEngine.cpp:121` (`InitializeLuaScriptInstance`, component scripts),
`:1302` (`LoadScript`, game.lua), `:1408` (`ParsePropertySchema`).

Add to `ScriptEngine` (`ScriptEngine.h`, near `:69`):
`void LoadScriptFromString(const std::string& code, const std::string& chunkName);`
impl: `m_lua->safe_script(code, sol::script_pass_on_error);`

- `LoadScript` (`:1302`): read `auto b = vfs::ReadAssetAbs(filePath);` if non-empty
  `m_lua->safe_script(std::string(b.begin(), b.end()), sol::script_pass_on_error)` else current
  `safe_script_file`. Boot/runtime call sites (`Application.cpp:315`, `:2281`) need no change.
- `InitializeLuaScriptInstance` (`:121`): replace `safe_script_file(abs.string(), env, ...)` with
  reading `vfs::ReadAsset(ls.scriptPath)` -> string -> `safe_script(code, env, ...)`; disk fallback to
  `safe_script_file` if empty.
- `ParsePropertySchema` (`:1408`): replace the `std::ifstream(abs.string())` block with
  `vfs::ReadAsset(rel)` -> `code`; rest unchanged.

### 9.6 Scene / prefab / sceneflow JSON
- `SceneSerializer::Load` (`src/scene/SceneSerializer.cpp:1157`): read `vfs::ReadAssetAbs(filePath)`;
  if non-empty `return LoadFromString(scene, std::string(b.begin(),b.end()), assetsDir)` (the
  `LoadFromString` overload already exists at `:1192`); else current ifstream path. One branch added.
- `SceneSerializer::InstantiatePrefab` (`:1460`): replace the ifstream block with `vfs::ReadAssetAbs`
  -> string -> existing `InstantiateSubtree(scene, str, assetsDir, outAll)` (`:1396`).
- `SceneFlow::Load` (`src/scene/SceneFlow.cpp:11`): add `bool LoadFromString(const std::string& json)`
  holding the current parse logic (lines 21-40); change `Load(path)` to read `vfs::ReadAsset("sceneflow.json")`
  and call `LoadFromString` when non-empty, else the existing ifstream. Call site `Application.cpp:750`
  unchanged.

### 9.7 Boot config (`game.json` replacement)
`game.json` is no longer shipped. In game mode, after `vfs::MountPak`, read `vfs::ReadBootConfig` for
`startScene`/window. Editor keeps reading loose `game.json`/project files via `Project::Load`
unchanged. Concretely: in `Application::Initialize` where `Project::Load(BaseDir()+"game.json")` runs
(`~:334`), guard with `if (vfs::InGameMode()) { vfs::ReadBootConfig(bc); startScene = bc.startScene; }
else { Project::Load(...); }`.

---

## 10. Game-only runtime (`GameRuntime`)

### 10.1 `src/main.cpp` edits

Wrap the mode-decision block so the runtime is unconditionally the game and exposes no editor/build/
validate path. Exact edits:

- Wrap `--validate` handling (`main.cpp:178-190`) in `#ifndef DX12_GAME_RUNTIME ... #endif`.
- Wrap the flag parsing for `--game/--build/--editor` (`:192-197`) in `#ifndef DX12_GAME_RUNTIME`.
- Wrap the `game.json`-next-to-exe check (`:202-210`) in `#ifndef DX12_GAME_RUNTIME`.
- Immediately after that block add:

```cpp
#ifdef DX12_GAME_RUNTIME
    gameMode  = true;   // compile-time hard guarantee: ALWAYS the game
    buildMode = false;
#endif
```

So in `GameRuntime`, `gameMode` is `true` before `PathResolver::Initialize(gameMode)` (`:213`), the
build branch (`:224`) is dead, and there is no code path that reads any file to decide the mode.
Deleting `game.pak`/`game.json` cannot turn it into the editor (a missing pak is a fatal "assets not
found" error, not an editor launch).

Add VFS mount in game mode. Best location: very early in `Application::Initialize` (before any asset
load) OR in `main.cpp` right after `PathResolver::Initialize`:

```cpp
#ifdef DX12_GAME_RUNTIME
    if (!dx12e::vfs::MountPak((std::filesystem::path(exeDir) / "game.pak").string()))
        throw std::runtime_error("game.pak not found or corrupt");
#endif
```

(Editor/DX12Engine never mounts a pak -> `vfs::InGameMode()==false` -> disk mode everywhere.)

### 10.2 Root `CMakeLists.txt` — new `GameRuntime` target

Insert after the `DX12Engine` target block (after line 62), before the DXC block:

```cmake
add_executable(GameRuntime WIN32 src/main.cpp)
target_link_libraries(GameRuntime PRIVATE
    Core Graphics Renderer Resource Animation ECS Audio Input Scene Scripting Physics Gui
    Editor ProjectLib)   # kept linked: dead code, unreachable (gameMode compile-forced true)
if(MSVC)
    target_compile_options(GameRuntime PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(GameRuntime PRIVATE ${DX12_COMPILE_DEFS} DX12_GAME_RUNTIME)
endif()
```

And after the `Shaders` target is defined (after line 379) add `add_dependencies(GameRuntime Shaders)`.

`Editor`/`ProjectLib` stay in the link list so the shared `Core`/`Application.cpp` references resolve
under `/WX` without compiling `Core` twice. The editor is unreachable, not absent (see §11 for the
optional strip).

### 10.3 `BuildGame` rewrite (`src/core/Application.cpp:2624`)

Replace steps 2, 3, 6 (and the exe copy in step 1) so the build ships `GameRuntime` + `game.pak`,
NOT the editor exe and NOT loose assets/scripts:

1. **Copy `GameRuntime.exe` as `Game.exe`** (not the running editor exe). It sits next to the editor
   exe in the build output dir:
   ```cpp
   fs::path exeDir = fs::path(exePathFromGetModuleFileName).parent_path();
   fs::path runtimeSrc = exeDir / "GameRuntime.exe";
   if (!fs::exists(runtimeSrc)) { Logger::Error("GameRuntime.exe missing; build it first"); return; }
   fs::copy_file(runtimeSrc, outputDir / "Game.exe", overwrite);
   ```
   Keep the DLL-copy loop (`:2645-2657`) unchanged.
2. **Delete steps 2 (scripts copy) and 3 (assets copy).** Replace with a single pack pass:
   ```cpp
   vfs::PakWriter w; w.Open((outputDir / "game.pak").string());
   for (auto& p : fs::recursive_directory_iterator(AssetsDir()))
       if (p.is_regular_file()) w.AddFile(p.path().string(), relTo(AssetsDir(), p.path()));
   for (auto& p : fs::recursive_directory_iterator(ScriptsDir()))   // scripts/ packed too
       if (p.is_regular_file()) w.AddFile(p.path().string(), "scripts/" + relTo(ScriptsDir(), p.path()));
   // boot manifest (replaces game.json)
   std::string manifest = makeManifestJson(startSceneRel /* computed as today, :2701-2720 */);
   w.AddBlob("__manifest__", (const uint8_t*)manifest.data(), manifest.size());
   w.Finish(/*stripStrings=*/true);   // release: strip string table
   ```
   Note: script keys are packed under `scripts/...` to match `PathResolver::GameLuaPath()` /
   `ls.scriptPath` resolution. Confirm `Normalize` strips only a leading `assets/`, leaving
   `scripts/` intact — scripts live OUTSIDE assets/. (Adjust `Normalize` to NOT strip `scripts/`.)
3. **Keep step 4 (shaders copy) unchanged** — `.cso` stay loose.
4. **Delete step 6 (`game.json` write).** Boot config now lives in the pak. (Optionally still write a
   tiny decoy `game.json`; not required.)
5. Step 5 (`Game.bat`) — change to `Game.exe` (no `--game` needed; GameRuntime ignores args). Keep or
   drop; harmless.

`BuildGameStandalone` (`:2615`) is unchanged (it sets start scene then calls `BuildGame`).

---

## 11. Optional later hardening (NOT required for v1)

To physically remove editor code from the game binary (smaller exe, no editor symbols to inspect):
compile a second `Core` variant `CoreGame` with `DX12_GAME_RUNTIME`, wrap the `EditorLayer`/
`EditorContext` construction in `Application.cpp` (`~:124`, `:673-685`) and member decls in
`#ifndef DX12_GAME_RUNTIME`, link `GameRuntime` against `CoreGame` and drop `Editor`/`ProjectLib`.
This is real `/WX` surgery across a 4700-line file — defer until v1 is proven. The v1 compile-time
mode force already makes the editor unreachable.

---

## 12. CMake change summary

- `src/core/CMakeLists.txt`: add the new `vfs/*.cpp` sources to `add_library(Core STATIC ...)`
  (`:3-10`); add `bcrypt.lib cabinet.lib` to `target_link_libraries(Core ... PRIVATE ...)` (`:20-23`);
  add a configure-time existence check for `generated/AssetKey.h` (FATAL_ERROR if missing).
- `src/resource/CMakeLists.txt`: add `VfsIOSystem.cpp` to `add_library(Resource STATIC ...)` (`:18-23`).
  Resource already links assimp + Core.
- Root `CMakeLists.txt`: add `GameRuntime` target + `add_dependencies(GameRuntime Shaders)` (§10.2).
- `.gitignore`: add `src/core/generated/`.
- `build_debug.bat` / `build_release.bat`: prepend `powershell -ExecutionPolicy Bypass -File
  tools/gen_asset_key.ps1` (no-op if header exists).
- `/WX`, `/utf-8`, `${DX12_COMPILE_OPTIONS}`, `${DX12_COMPILE_DEFS}` applied to `GameRuntime`
  identically to `DX12Engine` (parity).

---

## 13. Implementation order (respects dependencies)

1. **PakFormat.h** (`Normalize`, `FnvHash`, structs + static_asserts). No deps.
2. **gen_asset_key.ps1 + generated/AssetKey.h** + `.gitignore` + CMake existence check + build_*.bat.
3. **Crypto.{h,cpp}** (AES-GCM + AssembleKey) — link `bcrypt.lib`. Unit-test encrypt->decrypt round
   trip + tamper-detection (flip a byte -> decrypt returns false).
4. **Compression.{h,cpp}** (XPRESS_HUFF) — link `cabinet.lib`. Round-trip test.
5. **PakWriter + PakArchive** — first with **store-only, no encryption, no compression** (flags 0).
   Round-trip: pack a folder, mount, compare every entry bytes to source. THEN enable encryption,
   THEN compression (per research sequencing).
6. **Vfs.{h,cpp}** facade (disk mode + pak mode + ReadBootConfig + ReadAssetAbs prefix strip).
7. **Loader hooks** in this order (each independently testable in the editor in DISK mode, which must
   stay byte-identical): textures (§9.1), audio (§9.4), Lua (§9.5), JSON (§9.6), cubemap (§9.2).
8. **VfsIOSystem** + ModelLoader hook (§9.3) — the hard one; test split `.gltf`+`.bin`.
9. **main.cpp `DX12_GAME_RUNTIME` guards + VFS mount** (§10.1) + **GameRuntime CMake target** (§10.2).
10. **BuildGame rewrite** (§10.3) + boot config from pak (§9.7).
11. Full build/verify (§14).

Steps 1-7 leave the engine fully working in the editor (disk mode) at every point — land them first,
verify nothing regressed, then do the runtime/build steps.

---

## 14. Build & verify plan (Debug + Release, `/WX`)

Per project convention the lead builds BOTH configs; the USER does runtime/visual verification.

Build:
```
build_debug.bat      # runs gen_asset_key.ps1 (once) then cmake --build Debug
build_release.bat    # Release
```
Both must compile clean under `/W4 /WX` for `DX12Engine` AND `GameRuntime`.

Automated/headless checks the lead performs:
1. **Crypto/compression/pak unit tests** (add to `tests/`, GPU-free): AES round-trip, tamper ->
   false, XPRESS round-trip, pak store-only round-trip, pak encrypted+compressed round-trip
   (pack a temp dir, mount, assert every blob == source). Run via `ctest`.
2. **`--validate` regression**: `DX12Engine.exe --validate assets/scenes/title.json` still exits 0
   (disk mode, untouched).
3. **Editor smoke (disk mode)**: `DX12Engine.exe` (no args) launches the EDITOR and loads loose
   textures/models/audio/Lua/scenes exactly as before (no pak present -> `vfs::InGameMode()==false`).
4. **Build the game**: `DX12Engine.exe --build` (or editor toolbar) -> `build/game/` contains
   `Game.exe` (copy of `GameRuntime.exe`), `game.pak`, `shaders/`, DLLs. NO loose `assets/`,
   `scripts/`, or `game.json`.

USER visual verification (hand-off checklist):
- `build/game/Game.exe` double-clicked launches the GAME (not the editor), correct start scene,
  textures/models/skybox/audio/UI all present.
- **Delete `game.json` (none should exist) — and even delete other files: it stays the game.** Delete
  `game.pak` -> clean fatal error dialog ("game.pak not found or corrupt"), NOT an editor launch.
- Rename/extract attempt: `game.pak` is not a readable zip; `strings game.pak` shows no asset names
  (release strips the string table) and no plaintext key.
- BGM/SFX, a split-glTF model (`assets/models/human/walk.gltf`), a DDS skybox, and a Lua-driven UI
  image all load from the pak.

---

## 15. Backward compatibility (explicit)

- **Editor** runs in disk mode (no pak mounted): `vfs::ReadAsset` reads loose files; every loader's
  fallback path is preserved; behavior byte-identical to today.
- **`--validate`** is editor-exe only, disk mode, references the filesystem directly — untouched. It
  is compiled out of `GameRuntime`.
- **Existing projects** keep working: loose `assets/`, `scripts/`, `game.json`, scene/prefab JSON,
  split `.gltf`+`.bin` all load unchanged in the editor. The pak is produced only by `BuildGame`.
- **`PathResolver`** is unchanged; `ASSETS_DIR/SCRIPTS_DIR/SHADER_DIR` macros stay defined for `Core`
  at compile time (required so code compiles) but are unused at runtime in dist/game mode.
- **Shaders** stay loose `.cso` in both editor and game; the shader load path is never touched.
```
