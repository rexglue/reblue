#!/usr/bin/env bash
# Builds the AppImage.
#
# An AppImage only runs anywhere if it references no symbol version newer than
# its targets have. Building on a hotrod host silently produces one that runs
# locally and refuses to even start elsewhere, because the dynamic linker bails
# before any code runs. That is exactly what happened to an earlier build when
# it reached a Steam Deck. The scan below is what makes the ceiling real.
#
# In: PRESET, OUT_FILE, APPIMAGE_ARCH, MAX_GLIBC, MAX_GLIBCXX
set -euo pipefail

BUILD_DIR="out/build/${PRESET}"
STAGING="$(mktemp -d)"
APPDIR="${STAGING}/AppDir"
trap 'rm -rf "${STAGING}"' EXIT

# Shared libs sit beside the binary: reblue's INSTALL_RPATH is $ORIGIN, the
# same flat layout as the build dir and the Windows zip.
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/usr/lib"
cp "${BUILD_DIR}/reblue" "${BUILD_DIR}/gamecontrollerdb.txt" "${APPDIR}/usr/bin/"

shopt -s nullglob
libs=("${BUILD_DIR}"/librexruntime*.so*)
if [ ${#libs[@]} -eq 0 ]; then
  echo "::error::no librexruntime*.so in ${BUILD_DIR}"
  exit 1
fi
cp "${libs[@]}" "${APPDIR}/usr/bin/"

cp "${BUILD_DIR}"/libvdflib*.so* "${APPDIR}/usr/bin/" 2>/dev/null || true

if ldd "${APPDIR}/usr/bin/reblue" | grep -q "not found"; then
  ldd "${APPDIR}/usr/bin/reblue" | grep "not found" >&2
  echo "::error::unresolved runtime dependencies"
  exit 1
fi

# True when dotted version $1 is greater than $2.
version_gt() {
  if [ "$1" = "$2" ]; then
    return 1
  fi
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]
}

# "not found" above only catches missing libraries, not too-new symbol
# *versions* inside libraries that do resolve here. Scan everything bundled,
# including the SDK libs, whose toolchain this does not own.
violations=()
for f in "${APPDIR}/usr/bin"/*; do
  [ -f "${f}" ] || continue
  while IFS= read -r sym; do
    case "${sym}" in
      GLIBCXX_*)
        if version_gt "${sym#GLIBCXX_}" "${MAX_GLIBCXX}"; then
          violations+=("${f} requires ${sym} (ceiling GLIBCXX_${MAX_GLIBCXX})")
        fi
        ;;
      GLIBC_*)
        if version_gt "${sym#GLIBC_}" "${MAX_GLIBC}"; then
          violations+=("${f} requires ${sym} (ceiling GLIBC_${MAX_GLIBC})")
        fi
        ;;
    esac
  done < <(objdump -T "${f}" 2>/dev/null |
             grep -oE 'GLIBC(XX)?_[0-9]+[.][0-9]+([.][0-9]+)?' | sort -u)
done

if [ ${#violations[@]} -gt 0 ]; then
  printf '::error::%s\n' "${violations[@]}"
  echo "refusing to package: these would fail to start on older target systems." >&2
  echo "If one is a prebuilt SDK library, that SDK build is the actual problem." >&2
  exit 1
fi

# reblue and the SDK libs link libstdc++/libgcc dynamically, so vendor the
# build container's copies. glibc itself is never safe to vendor, since
# bundling it breaks NSS, DNS and locale resolution. The ceiling above is what
# makes relying on the target's glibc work instead.
for f in "${APPDIR}/usr/bin"/*; do
  [ -f "${f}" ] || continue
  ldd "${f}" 2>/dev/null | grep -E 'libstdc[+][+][.]so[.]6|libgcc_s[.]so[.]1' || true
done | awk '{print $3}' | sort -u | while IFS= read -r lib; do
  [ -f "${lib}" ] || continue
  cp "$(readlink -f "${lib}")" "${APPDIR}/usr/lib/$(basename "${lib}")"
  echo "vendoring $(basename "${lib}")"
done

cat > "${APPDIR}/AppRun" <<'APPRUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/reblue" "$@"
APPRUN
chmod +x "${APPDIR}/AppRun"

cat > "${APPDIR}/reblue.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=reblue
Exec=reblue
Icon=reblue
Categories=Game;
DESKTOP

cp res/reblue.png "${APPDIR}/reblue.png"
cp res/reblue.png "${APPDIR}/.DirIcon"

curl -fsSL -o "${STAGING}/appimagetool" \
  "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${APPIMAGE_ARCH}.AppImage"
chmod +x "${STAGING}/appimagetool"

# appimagetool is itself an AppImage, and a container has no FUSE for it to
# mount itself through. This makes it self-extract instead.
export APPIMAGE_EXTRACT_AND_RUN=1

mkdir -p dist
ARCH="${APPIMAGE_ARCH}" "${STAGING}/appimagetool" "${APPDIR}" "dist/${OUT_FILE}"
echo "Packaged dist/${OUT_FILE} ($(du -m "dist/${OUT_FILE}" | cut -f1) MB)"
