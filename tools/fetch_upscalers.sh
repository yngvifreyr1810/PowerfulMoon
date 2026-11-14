#!/bin/bash
# The vendors' upscaler runtimes the Windows release ships next to the exe (each loaded only when chosen in the menu),
# fetched from the vendors' own repositories at pinned revisions into build/upscalers/, each checked against its
# sha256 and its Authenticode signature (the vendor's), with the licence it comes under:
# - NVIDIA DLSS 310.9.1: nvngx_dlss.dll - github.com/NVIDIA/DLSS (NVIDIA RTX SDKs licence)
# - AMD FidelityFX SDK 2.4.0 (FSR 3.1 / FSR 4): amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll -
#   github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
# - Intel XeSS 2.0.2: libxess.dll - github.com/intel/xess
# make dist copies them (and the licences: tools/dist_licenses.sh). Skipped for a file already there with its hash.
set -euo pipefail
cd "$(dirname "$0")/.."
O=build/upscalers
mkdir -p "$O"

DLSS=https://github.com/NVIDIA/DLSS/raw/v310.9.1
AMD=https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX/signedbin
XESS=https://raw.githubusercontent.com/intel/xess/de0fb9c1c510661c571164e1418ceca8101dab69

signer() {  # file -> the subject of its valid Authenticode signature, or nothing
    powershell.exe -NoProfile -Command "\$s = Get-AuthenticodeSignature -LiteralPath '$(cygpath -w "$1")'; if (\$s.Status -eq 'Valid') { \$s.SignerCertificate.Subject }" | tr -d '\r'
}

get() {  # url sha256 file signer-pattern (empty: a text file, no signature)
    local f="$O/$3"
    if [ ! -f "$f" ] || [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$2" ]; then
        curl -fsSL --proto '=https' -o "$f.part" "$1"
        [ "$(sha256sum "$f.part" | cut -d' ' -f1)" = "$2" ] || { echo "$3: sha256 differs from the pinned one"; rm -f "$f.part"; exit 1; }
        mv "$f.part" "$f"
    fi
    if [ -n "$4" ]; then
        local s; s=$(signer "$f")
        [[ "$s" == *"$4"* ]] || { echo "$3: not signed by $4 (signature: ${s:-none})"; exit 1; }
    fi
}

get "$DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll" 3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983 \
    nvngx_dlss.dll "NVIDIA Corporation"
get "$DLSS/LICENSE.txt" "$(sha256sum third_party/dlss/LICENSE.txt | cut -d' ' -f1)" NVIDIA-RTX-SDK-LICENSE.txt ""
get "$AMD/amd_fidelityfx_loader_vk.dll" 38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9 \
    amd_fidelityfx_loader_vk.dll "Advanced Micro Devices"
get "$AMD/amd_fidelityfx_upscaler_vk.dll" 4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7 \
    amd_fidelityfx_upscaler_vk.dll "Advanced Micro Devices"
get "$XESS/bin/libxess.dll" 251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7 libxess.dll "Intel Corporation"
echo "upscalers: $(ls "$O" | tr '\n' ' ')"
