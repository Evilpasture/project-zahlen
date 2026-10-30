#!/usr/bin/env bash

set -euo pipefail

if ! command -v fontbm &> /dev/null; then
    echo "Error: 'fontbm' is not installed or not in PATH." >&2
    exit 1
fi

if ! command -v ninja &> /dev/null; then
    echo "Error: 'ninja' is not installed or not in PATH." >&2
    exit 1
fi

FONT_SIZE="${2:-32}"
DATA_FORMAT="${3:-json}"
OUTPUT_BASE_DIR="${4:-resources/fonts}"

# ------------------------------------------------------------------
# Platform-aware default font directories
# ------------------------------------------------------------------
if [[ "$(uname)" == "Darwin" ]]; then
    # macOS
    DEFAULT_FONT_DIRS=(
        "$HOME/Library/Fonts"
        "/Library/Fonts"
        "/System/Library/Fonts"
        "/System/Library/Fonts/Supplemental"
        "$HOME/.local/share/fonts"          # keep Linux-style path just in case
    )
    # Primary user dir used for single-name lookups
    DEFAULT_FONT_DIR="$HOME/Library/Fonts"
else
    # Linux
    DEFAULT_FONT_DIRS=(
        "/usr/share/fonts/TTF"
        "/usr/share/fonts"
        "$HOME/.local/share/fonts"
    )
    DEFAULT_FONT_DIR="$HOME/.local/share/fonts"
fi

to_pascal_case() {
    local input="$1"
    echo "$input" | sed -E 's/[^a-zA-Z0-9]+/ /g' | awk '{
        result = ""
        for (i=1; i<=NF; i++) {
            word = toupper(substr($i,1,1)) substr($i,2)
            result = result word
        }
        print result
    }'
}

declare -a FONT_PATHS=()
shopt -s nullglob

if [ "$#" -ge 1 ] && [ -n "$1" ]; then
    INPUT_TARGET="$1"
    TARGET_DIR=""

    if [ -d "$INPUT_TARGET" ]; then
        TARGET_DIR="$INPUT_TARGET"
    else
        # Try the platform default + a couple of common extra places
        for candidate in \
            "$DEFAULT_FONT_DIR/$INPUT_TARGET" \
            "$HOME/Library/Fonts/$INPUT_TARGET" \
            "/Library/Fonts/$INPUT_TARGET" \
            "$HOME/.local/share/fonts/$INPUT_TARGET"
        do
            if [ -d "$candidate" ]; then
                TARGET_DIR="$candidate"
                break
            fi
        done
    fi

    if [ -n "$TARGET_DIR" ]; then
        echo "Target directory identified: $TARGET_DIR"
        fonts=("$TARGET_DIR"/*.ttf "$TARGET_DIR"/*.otf "$TARGET_DIR"/*.ttc)
        if [ ${#fonts[@]} -eq 0 ]; then
            echo "Error: No .ttf / .otf / .ttc font files found in '$TARGET_DIR'." >&2
            exit 1
        fi
        for font in "${fonts[@]}"; do
            [ -f "$font" ] && FONT_PATHS+=("$font")
        done
    else
        FONT_PATH=""
        if [ -f "$INPUT_TARGET" ]; then
            FONT_PATH="$INPUT_TARGET"
        else
            for candidate in \
                "$DEFAULT_FONT_DIR/$INPUT_TARGET" \
                "$HOME/Library/Fonts/$INPUT_TARGET" \
                "/Library/Fonts/$INPUT_TARGET" \
                "$HOME/.local/share/fonts/$INPUT_TARGET"
            do
                if [ -f "$candidate" ]; then
                    FONT_PATH="$candidate"
                    break
                fi
            done
        fi

        if [ -n "$FONT_PATH" ]; then
            FONT_PATHS+=("$FONT_PATH")
        else
            echo "Error: Could not find '$INPUT_TARGET' as a file or directory." >&2
            exit 1
        fi
    fi
else
    echo "No target given – collecting fonts from:"
    for dir in "${DEFAULT_FONT_DIRS[@]}"; do
        echo "  • $dir"
    done
    echo ""

    for dir in "${DEFAULT_FONT_DIRS[@]}"; do
        if [ -d "$dir" ]; then
            fonts=("$dir"/*.ttf "$dir"/*.otf "$dir"/*.ttc)
            for font in "${fonts[@]}"; do
                [ -f "$font" ] && FONT_PATHS+=("$font")
            done
        fi
    done

    if [ ${#FONT_PATHS[@]} -eq 0 ]; then
        echo "Error: No .ttf / .otf / .ttc fonts found in the default locations." >&2
        exit 1
    fi
fi

# ------------------------------------------------------------------
# Temporary helper script that Ninja will call
# ------------------------------------------------------------------
HELPER=$(mktemp /tmp/find_license_XXXXXX.sh)
trap 'rm -f "$NINJA_FILE" "$HELPER"' EXIT

cat > "$HELPER" << 'HELPER_EOF'
#!/usr/bin/env bash
set -euo pipefail

font="$1"
out="$2"

dir=$(dirname -- "$font")
found=""

# 1. Look next to the font (and a couple of parents)
for d in "$dir" "$(dirname -- "$dir")" "$(dirname -- "$(dirname -- "$dir")")"; do
    for name in LICENSE LICENSE.txt LICENSE.md License.txt OFL.txt OFL COPYING COPYING.txt copyright COPYRIGHT licence LICENCE; do
        if [ -f "$d/$name" ]; then
            found="$d/$name"
            break 2
        fi
    done
done

# 2. Arch
if [ -z "$found" ] && command -v pacman >/dev/null; then
    pkg=$(pacman -Qo "$font" 2>/dev/null | awk '/is owned by/{print $5}' || true)
    if [ -n "$pkg" ]; then
        if [ -f "/usr/share/licenses/$pkg/LICENSE" ]; then
            found="/usr/share/licenses/$pkg/LICENSE"
        else
            found=$(find "/usr/share/licenses/$pkg" -type f 2>/dev/null | head -1 || true)
        fi
    fi
fi

# 3. Debian/Ubuntu
if [ -z "$found" ] && command -v dpkg >/dev/null; then
    pkg=$(dpkg -S "$font" 2>/dev/null | cut -d: -f1 | head -1 || true)
    if [ -n "$pkg" ] && [ -f "/usr/share/doc/$pkg/copyright" ]; then
        found="/usr/share/doc/$pkg/copyright"
    fi
fi

# 4. Homebrew (macOS / Linuxbrew)
if [ -z "$found" ] && command -v brew >/dev/null 2>&1; then
    cellar=$(brew --cellar 2>/dev/null || true)

    if [ -n "$cellar" ] && [[ "$font" == "$cellar"* ]]; then
        pkg=$(echo "$font" | sed -E "s|^${cellar}/([^/]+)/.*|\1|")

        if [ -n "$pkg" ]; then
            for cand in \
                "$cellar/$pkg"/*/LICENSE* \
                "$cellar/$pkg"/*/LICENCE* \
                "$cellar/$pkg"/*/COPYING* \
                "$cellar/$pkg"/*/copyright \
                "$cellar/$pkg"/*/share/doc/"$pkg"/LICENSE* \
                "$cellar/$pkg"/*/share/doc/"$pkg"/copyright
            do
                if [ -f "$cand" ]; then
                    found="$cand"
                    break
                fi
            done

            if [ -z "$found" ]; then
                license_str=$(brew info --json=v1 "$pkg" 2>/dev/null \
                    | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d[0].get("license") or "")' 2>/dev/null || true)

                if [ -n "$license_str" ]; then
                    cat > "$out" <<EOF
License information from Homebrew for formula "$pkg":

$license_str

(This is the license declared in the formula; no separate LICENSE file was found on disk.)
EOF
                    found="$out"
                    echo "  ✓ Homebrew license string used for $pkg"
                fi
            fi
        fi
    fi
fi

if [ -n "$found" ]; then
    # Avoid overwriting if we already wrote the Homebrew fallback above
    if [ "$found" != "$out" ]; then
        cp -- "$found" "$out"
    fi
    echo "  ✓ License found → $out"
else
    cat > "$out" <<EOF
All Rights Reserved.

No license file was found for this font.
The font is assumed to be proprietary / All Rights Reserved.
Do not redistribute without explicit permission from the copyright holder.
EOF
    echo "  ⚠ WARNING: No license found for $(basename -- "$font") — treating as All Rights Reserved" >&2
fi
HELPER_EOF

chmod +x "$HELPER"

# ------------------------------------------------------------------
# Generate Ninja file
# ------------------------------------------------------------------
NINJA_FILE=$(mktemp /tmp/fontbm_XXXXXX.ninja)

{
    cat <<EOF
# Auto-generated Ninja build file for fontbm + license bundling

rule fontbm
  command = fontbm --font-file \$in --output \$\$(dirname \$out)/\$\$(basename \$out .${DATA_FORMAT}) --font-size ${FONT_SIZE} --texture-size 1024x1024 --texture-crop-width --texture-crop-height --padding-up 4 --padding-down 4 --padding-left 4 --padding-right 4 --spacing-horiz 4 --spacing-vert 4 --data-format ${DATA_FORMAT}
  description = Generating atlas for \$in

rule find_license
  command = $HELPER \$in \$out
  description = Finding license for \$in
  restat = 1

EOF

    declare -A SEEN_DIRS=()

    ninja_escape() {
        local p=${1//\$/\$\$}
        p=${p// /\$ }
        printf '%s' "$p"
    }

    for font_path in "${FONT_PATHS[@]}"; do
        raw_basename=$(basename -- "$font_path")
        clean_name="${raw_basename%.*}"

        safe_name=$(echo "$clean_name" | sed -E 's/[^a-zA-Z0-9]+/_/g' | sed -E 's/^_|_$//g')
        [ -z "$safe_name" ] && safe_name="Font"

        pascal_dir=$(to_pascal_case "$clean_name")
        [ -z "$pascal_dir" ] && pascal_dir="$safe_name"

        if [[ -n ${SEEN_DIRS[$pascal_dir]+x} ]]; then
            parent=$(basename -- "$(dirname -- "$font_path")")
            parent_pascal=$(to_pascal_case "$parent")
            [ -z "$parent_pascal" ] && parent_pascal="Dup"

            candidate="${pascal_dir}_${parent_pascal}"

            if [[ -n ${SEEN_DIRS[$candidate]+x} ]]; then
                path_hash=$(echo -n "$font_path" | shasum -a 256 | cut -c1-6)
                candidate="${candidate}_${path_hash}"
            fi

            pascal_dir="$candidate"
        fi
        SEEN_DIRS[$pascal_dir]=1

        target_out_dir="$OUTPUT_BASE_DIR/$pascal_dir"
        mkdir -p "$target_out_dir"

        out_base="$target_out_dir/$safe_name"
        license_out="$target_out_dir/LICENSE.txt"

        esc_font=$(ninja_escape "$font_path")
        esc_out_json=$(ninja_escape "${out_base}.${DATA_FORMAT}")
        esc_license=$(ninja_escape "$license_out")

        echo "build ${esc_out_json}: fontbm ${esc_font}"
        echo "build ${esc_license}: find_license ${esc_font}"
        echo ""
    done
} > "$NINJA_FILE"

echo "Found ${#FONT_PATHS[@]} font(s). Generated Ninja build file: $NINJA_FILE"
echo "Running ninja (licenses will be resolved in parallel)..."
echo ""

ninja -f "$NINJA_FILE"

echo ""
echo "All font processing complete!"
