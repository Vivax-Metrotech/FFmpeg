#!/bin/sh
# Hardened FFmpeg dylib -> frameworks generator (continues on errors, logs summary)
set -u

ROOT="${1:-$(pwd)}"
INCLUDE_DIR="$ROOT/include"
LIB_DIR="$ROOT/lib"
OUT_DIR="$ROOT/Frameworks"

MIN_IOS_VERSION="${MIN_IOS_VERSION:-12.0}"
COPY_ALL_HEADERS="${COPY_ALL_HEADERS:-1}"   # 1 = copy full include tree into every framework

mkdir -p "$OUT_DIR"

cap_first(){ s="$1"; printf "%s%s" "$(printf "%.1s" "$s" | tr '[:lower:]' '[:upper:]')" "$(printf "%s" "$s" | cut -c2-)"; }
lower(){ printf "%s" "$1" | tr '[:upper:]' '[:lower:]'; }
strip_version(){ printf "%s" "$1" | sed -E 's/\.[0-9]+(\.[0-9]+)*$//'; }

# ---- target collection (dedupe by base: libfoo) ----
TARGET_COUNT=0
find_idx_by_base(){ base="$1"; i=0; while [ $i -lt ${TARGET_COUNT:-0} ]; do eval "b=\$TARGET_BASE_$i"; [ "$b" = "$base" ] && { echo "$i"; return; }; i=$((i+1)); done; echo "-1"; }
add_or_update_target(){
  path="$1"; file=$(basename "$path"); base=${file%.dylib}; base_no_ver=$(strip_version "$base")
  idx=$(find_idx_by_base "$base_no_ver")
  if [ "$idx" -lt 0 ]; then
    eval "TARGET_BASE_$TARGET_COUNT=\$base_no_ver"
    eval "TARGET_PATH_$TARGET_COUNT=\$path"
    TARGET_COUNT=$((TARGET_COUNT+1)); return
  fi
  eval "cur=\$TARGET_PATH_$idx"; curfile=$(basename "$cur")
  [ "$curfile" = "$base_no_ver.dylib" ] && return
  if [ "$file" = "$base_no_ver.dylib" ] || [ ${#file} -gt ${#curfile} ]; then
    eval "TARGET_PATH_$idx=\$path"
  fi
}

# ---- helpers (tolerant) ----
warn(){ printf "  [warn] %s\n" "$*" >&2; }
info(){ printf "  [info] %s\n" "$*"; }

copy_headers_tree(){
  src="$1"; fw="$2"
  if [ ! -d "$src" ]; then warn "headers not found: $src"; return 1; fi
  # shellcheck disable=SC2038
  (cd "$src" && find . -type f -name '*.h' -print) | while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    from="$src/$rel"; to="$fw/Headers/$rel"
    mkdir -p "$(dirname "$to")" || { warn "mkdir failed for $to"; return 1; }
    cp "$from" "$to" 2>/dev/null || { warn "cp failed: $from"; return 1; }
  done
  return 0
}

rewrite_peer_deps(){
  bin="$1"
  if ! otool -L "$bin" >/dev/null 2>&1; then warn "otool -L failed on $(basename "$bin")"; return 1; fi
  otool -L "$bin" | tail -n +2 | awk '{print $1}' | while IFS= read -r dep; do
    case "$dep" in
      *libavutil*.dylib)     new="@rpath/Avutil.framework/Avutil" ;;
      *libavcodec*.dylib)    new="@rpath/Avcodec.framework/Avcodec" ;;
      *libavformat*.dylib)   new="@rpath/Avformat.framework/Avformat" ;;
      *libavfilter*.dylib)   new="@rpath/Avfilter.framework/Avfilter" ;;
      *libavdevice*.dylib)   new="@rpath/Avdevice.framework/Avdevice" ;;
      *libswscale*.dylib)    new="@rpath/Swscale.framework/Swscale" ;;
      *libswresample*.dylib) new="@rpath/Swresample.framework/Swresample" ;;
      *) new="";;
    esac
    [ -n "$new" ] || continue
    [ "$dep" = "$new" ] && continue
    if ! install_name_tool -change "$dep" "$new" "$bin" 2>/dev/null; then
      warn "install_name_tool -change failed: $dep -> $new"
    else
      info "rewrote: $dep -> $new"
    fi
  done
  return 0
}

write_ios_modulemap(){
  name="$1"; mod="$2"
  EXCL='
libavcodec/d3d11va.h
libavcodec/dxva2.h
libavcodec/vaapi.h
libavcodec/vdpau.h
libavcodec/qsv.h
libavcodec/mediacodec.h
libavcodec/xvmc.h
libavutil/hwcontext_d3d11va.h
libavutil/hwcontext_dxva2.h
libavutil/hwcontext_qsv.h
libavutil/hwcontext_vaapi.h
libavutil/hwcontext_vdpau.h
libavutil/hwcontext_cuda.h
libavutil/hwcontext_drm.h
'
  mkdir -p "$(dirname "$mod")" || return 1
  {
    echo "framework module $name {"
    echo '  umbrella "Headers"'
    echo "$EXCL" | while IFS= read -r h; do [ -n "$h" ] && echo "  exclude header \"$h\""; done
    echo '  export *'
    echo '  module * { export * }'
    echo '}'
  } > "$mod"
}

make_framework(){
  base_no_ver="$1"; dylib="$2"
  name_base="${base_no_ver#lib}"; fw_name=$(cap_first "$name_base"); owned_sub="lib$(lower "$name_base")"
  FW_DIR="$OUT_DIR/$fw_name.framework"
  printf "==> Creating %s.framework from %s\n" "$fw_name" "$(basename "$dylib")"

  rm -rf "$FW_DIR" || true
  mkdir -p "$FW_DIR/Headers" "$FW_DIR/Modules" || { warn "mkdir framework dirs failed"; return 1; }

  # Copy binary
  if ! cp "$dylib" "$FW_DIR/$fw_name"; then warn "copy dylib failed"; return 1; fi
  chmod +x "$FW_DIR/$fw_name" || warn "chmod +x failed"

  # Headers
  if [ "${COPY_ALL_HEADERS}" = "1" ] && [ -d "$INCLUDE_DIR" ]; then
    copy_headers_tree "$INCLUDE_DIR" "$FW_DIR" || warn "header copy (full tree) issues"
  elif [ -d "$INCLUDE_DIR/$owned_sub" ]; then
    copy_headers_tree "$INCLUDE_DIR/$owned_sub" "$FW_DIR" || warn "header copy (subtree) issues"
  else
    warn "no headers found for $fw_name (continuing)"
  fi

  # Umbrella header (optional)
  printf '#import <Foundation/Foundation.h>\n' > "$FW_DIR/Headers/$fw_name.h" || warn "umbrella write failed"

  # Module map (iOS-safe)
  write_ios_modulemap "$fw_name" "$FW_DIR/Modules/module.modulemap" || warn "modulemap write failed"

  # Info.plist
  cat > "$FW_DIR/Info.plist" <<EOF || warn "Info.plist write failed"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$fw_name</string>
  <key>CFBundleIdentifier</key><string>com.vxmt.$fw_name</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleExecutable</key><string>$fw_name</string>
  <key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
  <key>MinimumOSVersion</key><string>$MIN_IOS_VERSION</string>
</dict></plist>
EOF

  # Set install name
  if ! install_name_tool -id "@rpath/$fw_name.framework/$fw_name" "$FW_DIR/$fw_name" 2>/dev/null; then
    warn "install_name_tool -id failed (continuing)"
  fi

  # Rewrite peer deps
  rewrite_peer_deps "$FW_DIR/$fw_name" || warn "peer dep rewrite issues"

  # Sign (best-effort)
  if command -v codesign >/dev/null 2>&1; then
    codesign -f -s - --timestamp=none "$FW_DIR" >/dev/null 2>&1 || warn "codesign failed"
  fi

  return 0
}

# ---- collect all dylibs ----
for f in "$LIB_DIR"/lib*.dylib; do [ -f "$f" ] && add_or_update_target "$f"; done
[ "${TARGET_COUNT:-0}" -gt 0 ] || { echo "No dylibs found in $LIB_DIR"; exit 1; }

echo "Targets to build:"
i=0; while [ $i -lt $TARGET_COUNT ]; do eval "b=\$TARGET_BASE_$i"; eval "p=\$TARGET_PATH_$i"; echo "  - $b  ->  $p"; i=$((i+1)); done

# ---- build all, never abort the whole run ----
FAIL_LIST=""
i=0
while [ $i -lt $TARGET_COUNT ]; do
  eval "b=\$TARGET_BASE_$i"; eval "p=\$TARGET_PATH_$i"
  if ! make_framework "$b" "$p"; then
    FAIL_LIST="$FAIL_LIST $b"
    echo "  [fail] $b"
  fi
  i=$((i+1))
done

echo "All done. Frameworks are in: $OUT_DIR"
[ -n "$FAIL_LIST" ] && echo "Some frameworks had issues:$FAIL_LIST"

