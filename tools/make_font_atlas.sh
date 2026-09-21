#!/usr/bin/env bash

set -euo pipefail

# Check if fontbm is installed
if ! command -v fontbm &> /dev/null; then
    echo "Error: 'fontbm' is not installed or not in PATH." >&2
    exit 1
fi

# Check if ninja is installed
if ! command -v ninja &> /dev/null; then
    echo "Error: 'ninja' is not installed or not in PATH." >&2
    exit 1
fi

FONT_SIZE="${2:-32}"
DATA_FORMAT="${3:-json}"
OUTPUT_BASE_DIR="${4:-resources/fonts}"
DEFAULT_FONT_DIR="$HOME/.local/share/fonts"
SYSTEM_TTF_DIR="/usr/share/fonts/TTF"

# Helper function to convert a string to true PascalCase
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

# Collect list of font paths to process
declare -a FONT_PATHS=()

# Enable nullglob so unmatched patterns resolve to empty
shopt -s nullglob

# ------------------------------------------------------------------
# Decide what to process
# ------------------------------------------------------------------
if [ "$#" -ge 1 ] && [ -n "$1" ]; then
    INPUT_TARGET="$1"

    # 1. Explicit directory
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
        # 2. Explicit single file
        FONT_PATH=""
        if [ -f "$INPUT_TARGET" ]; then
            FONT_PATH="$INPUT_TARGET"
        elif [ -f "$DEFAULT_FONT_DIR/$INPUT_TARGET" ]; then
            FONT_PATH="$DEFAULT_FONT_DIR/$INPUT_TARGET"
        fi

        if [ -n "$FONT_PATH" ]; then
            FONT_PATHS+=("$FONT_PATH")
        else
            echo "Error: Could not find '$INPUT_TARGET' as a file or directory in current path or '$DEFAULT_FONT_DIR'." >&2
            exit 1
        fi
    fi
else
    # ------------------------------------------------------------------
    # No argument → collect from both system + user locations by default
    # ------------------------------------------------------------------
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
# Generate a temporary Ninja build file
# ------------------------------------------------------------------
NINJA_FILE=$(mktemp /tmp/fontbm_XXXXXX.ninja)
trap 'rm -f "$NINJA_FILE"' EXIT

{
    echo "# Auto-generated Ninja build file for fontbm"
    echo "rule fontbm"
    echo "  command = fontbm --font-file \$in --output \$out --font-size ${FONT_SIZE} --texture-size 1024x1024 --texture-crop-width --texture-crop-height --padding-up 4 --padding-down 4 --padding-left 4 --padding-right 4 --spacing-horiz 4 --spacing-vert 4 --data-format ${DATA_FORMAT}"
    echo "  description = Generating atlas for \$in"
    echo ""

    for font_path in "${FONT_PATHS[@]}"; do
        raw_basename=$(basename -- "$font_path")
        clean_name="${raw_basename%.*}"

        pascal_dir=$(to_pascal_case "$clean_name")
        if [ -z "$pascal_dir" ]; then
            pascal_dir="$clean_name"
        fi

        target_out_dir="$OUTPUT_BASE_DIR/$pascal_dir"
        mkdir -p "$target_out_dir"

        out_base="$target_out_dir/$clean_name"
        echo "build ${out_base}.${DATA_FORMAT}: fontbm $font_path"
        echo ""
    done
} > "$NINJA_FILE"

echo "Found ${#FONT_PATHS[@]} font(s). Generated Ninja build file: $NINJA_FILE"
echo "Running ninja..."
echo ""

ninja -f "$NINJA_FILE"

echo ""
echo "All font processing complete!"
# Cleanup via trap
