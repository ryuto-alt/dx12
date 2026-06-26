# gen_asset_key.ps1
#
# Generates a random 32-byte asset encryption key per build, splits it into four
# 8-byte fragments, XOR-masks each fragment with a random 8-byte mask, and writes
# src/core/generated/AssetKey.h.
#
# NO-OP if the header already exists (so Debug/Release/packer/GameRuntime share one
# key). Each fresh clone / clean build gets a unique key.

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$genDir    = Join-Path $scriptDir "..\src\core\generated"
$header    = Join-Path $genDir "AssetKey.h"

if (Test-Path $header) {
    Write-Host "gen_asset_key: AssetKey.h already exists, skipping."
    exit 0
}

New-Item -ItemType Directory -Force -Path $genDir | Out-Null

$rng  = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$key  = New-Object byte[] 32
$mask = New-Object byte[] 32
$rng.GetBytes($key)
$rng.GetBytes($mask)

# frag[i] = key[i] XOR mask[i]  (C++ reconstructs key = frag XOR mask)
$frag = New-Object byte[] 32
for ($i = 0; $i -lt 32; $i++) {
    $frag[$i] = [byte]($key[$i] -bxor $mask[$i])
}

# 8 bytes -> little-endian uint64 (matches C++ memcpy little-endian).
function Chunk([byte[]]$bytes, [int]$idx) {
    return [System.BitConverter]::ToUInt64($bytes, $idx * 8)
}

$m0 = Chunk $mask 0; $f0 = Chunk $frag 0
$m1 = Chunk $mask 1; $f1 = Chunk $frag 1
$m2 = Chunk $mask 2; $f2 = Chunk $frag 2
$m3 = Chunk $mask 3; $f3 = Chunk $frag 3

$lines = @(
    "#pragma once",
    "#include <cstdint>",
    "// AUTO-GENERATED -- DO NOT COMMIT. Regenerated only if absent (stable across Debug/Release).",
    "namespace dx12e::vfs::detail {",
    ("inline constexpr uint64_t kMask0 = 0x{0:X16}ULL; inline constexpr uint64_t kFrag0 = 0x{1:X16}ULL;" -f $m0, $f0),
    ("inline constexpr uint64_t kMask1 = 0x{0:X16}ULL; inline constexpr uint64_t kFrag1 = 0x{1:X16}ULL;" -f $m1, $f1),
    ("inline constexpr uint64_t kMask2 = 0x{0:X16}ULL; inline constexpr uint64_t kFrag2 = 0x{1:X16}ULL;" -f $m2, $f2),
    ("inline constexpr uint64_t kMask3 = 0x{0:X16}ULL; inline constexpr uint64_t kFrag3 = 0x{1:X16}ULL;" -f $m3, $f3),
    "} // namespace dx12e::vfs::detail"
)

Set-Content -Path $header -Value $lines -Encoding ASCII
Write-Host "gen_asset_key: wrote $header"
exit 0
