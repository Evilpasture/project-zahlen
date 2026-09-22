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
DEFAULT_FONT_DIR="$HOME/.local/share/fonts"
SYSTEM_TTF_DIR="/usr/share/fonts/TTF"

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
    elif [ -d "$DEFAULT_FONT_DIR/$INPUT_TARGET" ]; then
        TARGET_DIR="$DEFAULT_FONT_DIR/$INPUT_TARGET"
    fi

    if [ -n "$TARGET_DIR" ]; then
        echo "Target directory identified: $TARGET_DIR"
        fonts=("$TARGET_DIR"/*.ttf "$TARGET_DIR"/*.otf)
        if [ ${#fonts[@]} -eq 0 ]; then
            echo "Error: No .ttf or .otf font files found in '$TARGET_DIR'." >&2
            exit 1
        fi
        for font in "${fonts[@]}"; do
            FONT_PATHS+=("$font")
        done
    else
        FONT_PATH=""
        if [ -f "$INPUT_TARGET" ]; then
            FONT_PATH="$INPUT_TARGET"
        elif [ -f "$DEFAULT_FONT_DIR/$INPUT_TARGET" ]; then
            FONT_PATH="$DEFAULT_FONT_DIR/$INPUT_TARGET"
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
    echo "  • $SYSTEM_TTF_DIR"
    echo "  • $DEFAULT_FONT_DIR"
    echo ""

    for dir in "$SYSTEM_TTF_DIR" "$DEFAULT_FONT_DIR"; do
        if [ -d "$dir" ]; then
            fonts=("$dir"/*.ttf "$dir"/*.otf)
            for font in "${fonts[@]}"; do
                FONT_PATHS+=("$font")
            done
        fi
    done

    if [ ${#FONT_PATHS[@]} -eq 0 ]; then
        echo "Error: No .ttf or .otf fonts found in the default locations." >&2
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

if [ -n "$found" ]; then
    cp -- "$found" "$out"
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
  command = fontbm --font-file \$in --output \$out --font-size ${FONT_SIZE} --texture-size 1024x1024 --texture-crop-width --texture-crop-height --padding-up 4 --padding-down 4 --padding-left 4 --padding-right 4 --spacing-horiz 4 --spacing-vert 4 --data-format ${DATA_FORMAT}
  description = Generating atlas for \$in

rule find_license
  command = $HELPER \$in \$out
  description = Finding license for \$in
  restat = 1

EOF

    for font_path in "${FONT_PATHS[@]}"; do
        raw_basename=$(basename -- "$font_path")
        clean_name="${raw_basename%.*}"

        pascal_dir=$(to_pascal_case "$clean_name")
        [ -z "$pascal_dir" ] && pascal_dir="$clean_name"

        target_out_dir="$OUTPUT_BASE_DIR/$pascal_dir"
        mkdir -p "$target_out_dir"

        out_base="$target_out_dir/$clean_name"
        license_out="$target_out_dir/LICENSE.txt"

        echo "build ${out_base}.${DATA_FORMAT}: fontbm $font_path"
        echo "build ${license_out}: find_license $font_path"
        echo ""
    done
} > "$NINJA_FILE"

echo "Found ${#FONT_PATHS[@]} font(s). Generated Ninja build file: $NINJA_FILE"
echo "Running ninja (licenses will be resolved in parallel)..."
echo ""

ninja -f "$NINJA_FILE"

echo ""
echo "All font processing complete!"
