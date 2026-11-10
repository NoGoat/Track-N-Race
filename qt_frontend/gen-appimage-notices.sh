#!/usr/bin/env bash
# Writes the licence notices for the host libraries linuxdeploy bundled into an
# AppDir's usr/lib, so the AppImage carries notices that match exactly what it
# ships. Each library is traced back to its distro package; the package's
# copyright file is reproduced, followed by every common licence those files
# refer to. Debian/Ubuntu (dpkg) hosts only.
#
# Usage: ./gen-appimage-notices.sh <AppDir> <output.txt>
set -euo pipefail

APPDIR="$1"
OUT="$2"

if ! command -v dpkg-query >/dev/null 2>&1; then
    echo "WARNING: dpkg not found — can't generate the bundled-library notices." >&2
    exit 1
fi

. /etc/os-release
sep() { printf '\n\n================================================================================\n%s\n================================================================================\n\n' "$1"; }

# library -> binary package (resolved through the host copy linuxdeploy took it from)
declare -A pkg_of
for f in "$APPDIR"/usr/lib/*.so*; do
    [[ -L "$f" ]] && continue
    lib="$(basename "$f")"
    host="$(ldconfig -p | awk -v n="$lib" '$1==n && !seen {print $NF; seen=1}')"
    [[ -z "$host" ]] && host="$(ls /usr/lib/*-linux-gnu/"$lib" 2>/dev/null | head -1 || true)"
    pkg=""
    [[ -n "$host" ]] && pkg="$(dpkg -S "$(readlink -f "$host")" 2>/dev/null | head -1 | cut -d: -f1-2 || true)"
    # Not on the linker path (e.g. libproxy's private libpxbackend): match by name.
    [[ -z "$pkg" ]] && pkg="$(dpkg -S "*/$lib" 2>/dev/null | head -1 | cut -d: -f1-2 || true)"
    if [[ -z "$pkg" ]]; then
        echo "WARNING: no package owns $lib — it won't appear in the notices." >&2
        continue
    fi
    pkg_of["$lib"]="$pkg"
done

# binary package -> "source version" and copyright file, one copyright per source package
declare -A src_of copyright_of
for pkg in $(printf '%s\n' "${pkg_of[@]}" | sort -u); do
    src="$(dpkg-query -W -f='${source:Package} ${source:Version}' "$pkg")"
    src_of["$pkg"]="$src"
    [[ -z "${copyright_of[$src]:-}" && -f "/usr/share/doc/${pkg%%:*}/copyright" ]] && \
        copyright_of["$src"]="/usr/share/doc/${pkg%%:*}/copyright"
done

common=$(printf '%s\n' "${copyright_of[@]}" | xargs grep -ohE "/usr/share/common-licenses/[A-Za-z0-9.+_-]+" \
         | sed 's|.*/||; s/[.,]$//' | sort -u)

{
    cat <<EOF
Linux AppImage libraries

The Linux AppImage of Track N Race's native app is built on $PRETTY_NAME
and bundles the unmodified libraries listed below, so the app runs on other
distributions. (The Windows build does not include them.)

Each library is used under its own licence. The copyright file for every
source package follows the list, then the full text of each common licence
those files refer to.

Source code for every package at the exact version listed is available from
the distribution, e.g. https://launchpad.net/ubuntu/+source/<package>/<version>,
or with "apt-get source <package>=<version>".


BUNDLED LIBRARIES (library -> source package and version)
---------------------------------------------------------

EOF
    for lib in "${!pkg_of[@]}"; do
        printf '  %-34s %s\n' "$lib" "${src_of[${pkg_of[$lib]}]}"
    done | sort
    for src in $(printf '%s\n' "${!copyright_of[@]}" | sort | tr ' ' '|'); do
        src="${src//|/ }"
        sep "$src"
        cat "${copyright_of[$src]}"
    done
    for c in $common; do
        [[ -f "/usr/share/common-licenses/$c" ]] || continue
        sep "/usr/share/common-licenses/$c"
        cat -- "$(readlink -f "/usr/share/common-licenses/$c")"
    done
} > "$OUT"

echo "Wrote bundled-library notices: $OUT"
