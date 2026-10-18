#!/usr/bin/env bash
# bbport PS5 - link the title and package its folder (run by ps5/CMakeLists.txt's title target).
#
#   link-title.sh BUILD_DIR PS5_VULKAN_DIR SDK OBJECT...
#
# The link is PS5_Vulkan's, as its titles and the PS5 Vulkan Template link: the RADV release
# archive, the link recipe (tools/radv-link.sh: the SDK's platform layer, the heap and thread
# wraps, the libc names bound to ps5_*), the CRT, the native tool (ELF -> fake SELF) and libc.prx.
# Every static library of this build is linked in one group with the loader's objects.
#
# Output: BUILD_DIR/dist/<TITLE_ID>/ with eboot.bin, sce_sys/ and sce_module/libc.prx.
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
[[ -n ${LINK_TRACE:-} ]] && set -x

work=$1 vulkan=$2 sdk=$3
shift 3
objects=("$@")
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || command -v clang-18 || true)}
tool="$vulkan/build/host/ps5-native-tool"
native="$vulkan/tooling/native"
param="$ps5/sce_sys/param.json"

for file in "$archive" "$tool" "$param" "$sdk/bin/prospero-lld" "$vulkan/tools/radv-link.sh" \
        "$vulkan/runtime/libc.prx"; do
    [[ -e $file ]] || { echo "missing: $file" >&2; exit 2; }
done
mkdir -p "$work/link/obj" "$work/link/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

# The CRT, and the C++ runtime from libc++abi (the GPU library throws and catches).
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/link/obj/app_crt.o"
# RADV calls AGC, which the SDK has no stubs for: these name its imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vulkan/$source" -o "$work/link/obj/${library}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/link/stubs/${library}.so" "$work/link/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2

# This build's static libraries (the GPU library, fmt, Zydis, sirit, FSR, LibAtrac9, ...).
mapfile -t libraries < <(find "$work" -name '*.a' -not -path "$work/link/*" | sort)
echo "link-title.sh: ${#objects[@]} objects, ${#libraries[@]} libraries" >&2

"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr --gc-sections "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/link/llvm-pie.elf" \
    "$work/link/obj/app_crt.o" "${objects[@]}" \
    --start-group "${libraries[@]}" "$sdk/target/lib/libc++experimental.a" --end-group \
    "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so

# Imports no module a title loads exports are null at run time (their first call jumps to 0):
# refuse them here, not on the console (as the PS5 Vulkan Template's link does).
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$work/link/llvm-pie.elf" |
        awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk"/target/lib/*.so "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so"; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "link-title.sh: imports that no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
    exit 1
fi
"$tool" link --in "$work/link/llvm-pie.elf" --out "$work/eboot.elf.new" \
    --stub-dir "$sdk/target/lib" --stub "$work/link/stubs/libSceAgc.so" \
    --stub "$work/link/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
app="$work/dist/$title_id"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/bbport"
"$tool" self --sign --in "$work/eboot.elf.new" --out "$app/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp "$param" "$app/sce_sys/param.json"
for asset in icon0.png pic0.png pic1.png; do
    [[ -f $ps5/sce_sys/$asset ]] && cp "$ps5/sce_sys/$asset" "$app/sce_sys/$asset"
done
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$app/sce_module/libc.prx"
mv "$work/eboot.elf.new" "$work/eboot.elf"
printf '==> %s: %s (eboot.bin %s bytes)\n' "$title_id" "$app" "$(stat -c %s "$app/eboot.bin")"
