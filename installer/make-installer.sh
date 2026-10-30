#!/bin/bash
# Builds PhiBrowserSetup-<version>.<build>.exe (x64) or
# PhiBrowserSetup-<version>.<build>-arm64.exe
# on THIS machine: the payload is staged straight from the ship out dir into
# installer/stage/<arch> in the installed layout, and makensis (the Debian
# `nsis` package) packs it. The Windows box plays no part.
#
# Usage: make-installer.sh [--version X.Y.Z] [--build-number N]
#                          [--arch x64|arm64] [--out <out dir>]
#                          [--telemetry phi_telemetry.json | --no-telemetry]
#                          [--no-sign] [--vmp]
#   --version    defaults to PHI_WIN_VERSION_STRING in client/phi_version.h
#   --build-number  defaults to PHI_WIN_VERSION_BUILD in client/phi_version.h;
#                   stamps the setup EXE's Windows file/product version.
#   --arch       which build to package: x64 (default) from out/PhiFxWinShip,
#                arm64 from out/PhiFxWinShipArm64 (docs/static-build-plan.md
#                §10). The tree's Phi.exe and chrome.dll must be of that
#                architecture (checked on their PE headers).
#   --out        another out dir to package instead of the arch's default
#   --telemetry  the deployment config the installer drops into the user
#                data dir (crash_endpoint, analytics_*; docs/sentry-crash-
#                reporting.md). Default: rendered by the sibling
#                ../../phibrowser-win-build/push-telemetry-config.sh --print
#                when that repo is checked out beside this one,
#                else none. --no-telemetry builds a config-less installer
#                (crashes stay local, analytics never leave the machine).
#   --no-sign    skip Authenticode. Otherwise every PE in the stage, the
#                setup exe and the uninstaller are signed by the sibling
#                ../../phibrowser-win-build/sign-pe.sh with the identity in
#                its phi-signing.env (the LAN rig's TEST chain or the release
#                one; docs/static-build-plan.md) - when that env exists.
#                Without it the build is unsigned and says so.
#   --vmp        Widevine VMP-sign Phi.exe and framework\chrome.dll with the
#                castLabs EVS client (python3 -m pip install castlabs-evs; an
#                EVS account with Widevine 3PL access, EVS_ACCOUNT_NAME +
#                EVS_PASSWD or a cached `python3 -m castlabs_evs.account
#                reauth`). Fails the build if signing fails. Without it the
#                CDM still plays, unverified, and services that demand VMP
#                refuse a license.
#
# The staged tree (docs/static-build-plan.md §6): Phi.exe, WinSparkle.dll and
# the client's own resources\ entries at the top (the list build-client.py
# writes to resources\phi-client-resources.txt), everything else under
# framework\ - chrome.dll, chrome_elf.dll + chrome_wer.dll, the paks, locales,
# ICU, the GL/Vulkan/DXC DLLs, Chromium's resources\, and the one manifest
# (framework\framework.manifest) that lets the exe's static chrome_elf.dll
# import resolve from inside the folder. The file set is the same one
# sync-fx-win.sh ships. Every PE must be the tree's architecture; the x64
# msdia140.dll that vs_toolchain.py copies into every out dir (dump_syms /
# symupload tooling, nothing in Phi loads it) is dropped from an arm64
# payload, anything else foreign is a packaging mistake.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT="${PHI_SRC_ROOT:-$(cd "$HERE/../.." && pwd)}"
VERSION=""
BUILD_NUMBER=""
ARCH=x64
OUT=""
TELEMETRY=""
NO_TELEMETRY=no
NO_SIGN=no
VMP=no
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --build-number) BUILD_NUMBER="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --telemetry) TELEMETRY="$2"; shift 2 ;;
    --no-telemetry) NO_TELEMETRY=yes; shift ;;
    --no-sign) NO_SIGN=yes; shift ;;
    --vmp) VMP=yes; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$VERSION" ]]; then
  VERSION=$(grep -oP '(?<=^#define PHI_WIN_VERSION_STRING ")[^"]+' \
    "$HERE/../client/phi_version.h")
fi
if [[ -z "$BUILD_NUMBER" ]]; then
  BUILD_NUMBER=$(grep -oP '(?<=^#define PHI_WIN_VERSION_BUILD )\d+' \
    "$HERE/../client/phi_version.h")
fi
[[ "$BUILD_NUMBER" =~ ^(0|[1-9][0-9]*)$ && ${#BUILD_NUMBER} -le 5 ]] &&
  (( BUILD_NUMBER <= 65535 )) || {
    echo "--build-number must be an integer from 0 to 65535" >&2; exit 2; }
case "$ARCH" in
  x64) SUFFIX=""; : "${OUT:=$SRC_ROOT/out/PhiFxWinShip}" ;;
  arm64) SUFFIX="-arm64"; : "${OUT:=$SRC_ROOT/out/PhiFxWinShipArm64}" ;;
  *) echo "--arch must be x64 or arm64" >&2; exit 2 ;;
esac
[[ -f "$OUT/Phi.exe" && -f "$OUT/chrome.dll" ]] || {
  echo "$OUT has no Phi.exe + chrome.dll (build-fx.sh chrome phi_client, then build-fx-client.sh)" >&2; exit 1; }
command -v makensis >/dev/null || { echo "makensis not found (apt install nsis)" >&2; exit 1; }
FULL_VERSION="$VERSION.$BUILD_NUMBER"
SETUP="PhiBrowserSetup-$FULL_VERSION$SUFFIX.exe"
STAGE="$HERE/stage/$ARCH"
DIST="$HERE/dist"
echo "version: $FULL_VERSION  arch: $ARCH  out: $OUT"

# The telemetry config: an explicit file, else the rendered deployment
# config, else nothing. The rendered file is a throwaway in the temp dir so
# the keys never sit in the repo.
TELEMETRY_DEFINE=()
if [[ "$NO_TELEMETRY" == no ]]; then
  TELEMETRY_TOOL="$HERE/../../phibrowser-win-build/push-telemetry-config.sh"
  if [[ -z "$TELEMETRY" && -x "$TELEMETRY_TOOL" ]]; then
    TELEMETRY="$(mktemp -t phi_telemetry.XXXXXX.json)"
    "$TELEMETRY_TOOL" --print > "$TELEMETRY"
  fi
  if [[ -n "$TELEMETRY" ]]; then
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); \
      print("telemetry:", ", ".join(sorted(d)) or "(empty)")' "$TELEMETRY"
    TELEMETRY_DEFINE=("-DPHI_TELEMETRY_JSON=$(realpath "$TELEMETRY")")
  else
    echo "telemetry: none (no --telemetry and no $TELEMETRY_TOOL)"
  fi
else
  echo "telemetry: none (--no-telemetry)"
fi

# The machine a PE was built for, off its COFF header.
pe_machine() {
  python3 - "$1" <<'EOF'
import struct, sys
with open(sys.argv[1], 'rb') as f:
    d = f.read(4096)
pe = struct.unpack_from('<I', d, 0x3c)[0]
m = struct.unpack_from('<H', d, pe + 4)[0]
print({0x8664: 'x64', 0xAA64: 'arm64', 0x14c: 'x86'}.get(m, hex(m)))
EOF
}
tree_arch=$(pe_machine "$OUT/Phi.exe")
dll_arch=$(pe_machine "$OUT/chrome.dll")
[[ "$tree_arch" == "$dll_arch" ]] || { echo "$OUT mixes a $tree_arch Phi.exe with a $dll_arch chrome.dll" >&2; exit 1; }
[[ "$tree_arch" == "$ARCH" ]] || { echo "$OUT is a $tree_arch tree, --arch $ARCH asked" >&2; exit 1; }

# Stage, from scratch every time.
echo "staging payload into $STAGE..."
rm -rf "$STAGE"
FW="$STAGE/framework"
mkdir -p "$STAGE/resources" "$FW"
RUNTIME_EXES=(Phi.exe chrome_proxy.exe chrome_pwa_launcher.exe
              elevation_service.exe elevated_tracing_service.exe
              notification_helper.exe)
TOP=(Phi.exe WinSparkle.dll)
CLIENT_LIST="$OUT/resources/phi-client-resources.txt"
[[ -f "$CLIENT_LIST" ]] || { echo "$CLIENT_LIST missing - rebuild the client (build-client.py writes it)" >&2; exit 1; }
mapfile -t CLIENT < <(grep -v '^\s*$' "$CLIENT_LIST")
cd "$OUT"
# Top level: the client's own.
for f in "${TOP[@]}"; do [[ -f "$f" ]] && cp -p "$f" "$STAGE/"; done
for e in "${CLIENT[@]}"; do
  [[ -e "resources/$e" ]] && cp -rp "resources/$e" "$STAGE/resources/$e"
done
# framework\: every other runtime file, then Chromium's share of resources.
for exe in "${RUNTIME_EXES[@]}"; do
  [[ -f "$exe" && "$exe" != Phi.exe ]] && cp -p "$exe" "$FW/"
done
find . -maxdepth 1 -type f \( -name '*.dll' -o -name '*.pak' -o -name '*.bin' \
    -o -name '*.dat' -o -name '*.manifest' \) -printf '%P\n' | while read -r f; do
  [[ "$f" == WinSparkle.dll ]] && continue
  case "$f" in
    *.manifest) [[ "$f" == framework.manifest ]] || continue ;;
  esac
  cp -p "$f" "$FW/"
done
for d in Dictionaries locales MEIPreload resources; do
  [[ -d "$d" ]] && cp -rp "$d" "$FW/$d"
done
for e in "${CLIENT[@]}" phi-client-resources.txt; do
  rm -rf "$FW/resources/$e"
done
[[ -f "$FW/framework.manifest" ]] || {
  echo "$OUT has no framework.manifest - a framework built before the assembly rename (chrome/app/version_assembly)" >&2; exit 1; }
# Every PE the tree's architecture; the one known foreign file dropped.
while IFS= read -r pe; do
  m=$(pe_machine "$pe")
  [[ "$m" == "$ARCH" ]] && continue
  if [[ "$(basename "$pe")" == msdia140.dll ]]; then rm -f "$pe"; continue; fi
  echo "$pe is $m in a $ARCH payload" >&2; exit 1
done < <(find "$STAGE" -type f \( -name '*.exe' -o -name '*.dll' \))
N=$(find "$STAGE" -type f | wc -l)
echo "staged layout=framework arch=$ARCH files=$N"

# Authenticode. The payload's PEs are signed here in the stage; the setup
# exe and the uninstaller stub go through the script's !finalize /
# !uninstfinalize with the same tool (-DPHI_SIGN_CMD) - the uninstaller
# exists only inside makensis before it is packed, so that is the one place
# it can be signed. Signing a PE leaves its CodeView debug id alone, so
# Sentry's symbols still match.
SIGN_TOOL="$HERE/../../phibrowser-win-build/sign-pe.sh"
SIGN_DEFINE=()
if [[ "$NO_SIGN" == yes ]]; then
  echo "signing: none (--no-sign)"
elif [[ -x "$SIGN_TOOL" ]] && SIGN_ID=$("$SIGN_TOOL" --info 2>/dev/null); then
  echo "signing: $SIGN_ID"
  find "$STAGE" -type f \( -name '*.exe' -o -name '*.dll' \) -print0 \
    | xargs -0 "$SIGN_TOOL"
  SIGN_DEFINE=("-DPHI_SIGN_CMD=$SIGN_TOOL")
else
  echo "signing: none (no $SIGN_TOOL with a phi-signing.env)"
fi

# Widevine VMP: a .sig beside Phi.exe and framework\chrome.dll, the host
# files the CDM verifies (the fork's chrome/common/media/cdm_host_file_path.cc).
# On Windows it must come after Authenticode - the VMP signature covers the
# code-signed file.
if [[ "$VMP" == yes ]]; then
  for pe in "$STAGE/Phi.exe" "$FW/chrome.dll"; do
    EVS_NO_ASK=1 python3 -m castlabs_evs.vmp sign "$pe" "$pe.sig"
    [[ -s "$pe.sig" ]] || { echo "EVS wrote no $pe.sig" >&2; exit 1; }
  done
  echo "vmp: Phi.exe.sig, framework/chrome.dll.sig"
else
  echo "vmp: none (no --vmp; Widevine runs unverified)"
fi

# The window's look comes from the PhiSetupUI plugin (installer/ui, the
# committed x86-unicode DLL; build-plugin.sh rebuilds it from the .c file).
# makensis on POSIX maps the script's backslashed paths onto these
# directories; the script is UTF-8 (the "…" and "·" in its strings).
[[ -f "$HERE/ui/x86-unicode/PhiSetupUI.dll" ]] || "$HERE/ui/build-plugin.sh"
mkdir -p "$DIST"
echo "running makensis..."
(makensis -V2 -INPUTCHARSET UTF8 \
  "-DPHI_VERSION=$VERSION" "-DPHI_BUILD_NUMBER=$BUILD_NUMBER" \
  "-DPHI_ARCH=$ARCH" "-DPHI_STAGE=$STAGE" \
  "-DPHI_OUTDIR=$DIST" -DPHI_LAYOUT_FRAMEWORK \
  "${TELEMETRY_DEFINE[@]}" "${SIGN_DEFINE[@]}" "$HERE/phi-installer.nsi") | tail -3
[[ -f "$DIST/$SETUP" ]] || { echo "makensis produced no $DIST/$SETUP" >&2; exit 1; }
[[ ${#SIGN_DEFINE[@]} -eq 0 ]] || "$SIGN_TOOL" --verify "$DIST/$SETUP"
ls -la "$DIST/$SETUP"
sha256sum "$DIST/$SETUP" | cut -c1-16
