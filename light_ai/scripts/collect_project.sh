#!/usr/bin/env bash
# Collect project files into a single text dump.
# Usage:
#   ./scripts/collect_project.sh [output_file]           # full project (default: project_dump.txt)
#   ./scripts/collect_project.sh --core [output_file]    # core only (no tools/ and externals/)

set -euo pipefail

# If located in scripts/, step up to project root; otherwise use current dir.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "$(basename "${SCRIPT_DIR}")" == "scripts" ]]; then
    ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
else
    ROOT="$(pwd)"
fi
cd "${ROOT}"

MODE="full"
if [[ "${1:-}" == "--core" ]]; then
    MODE="core"
    shift
fi

OUT="${1:-project_dump.txt}"
# Resolve relative path for reliable self-exclusion
OUT_REL="${OUT#./}"

collect_files() {
    if [[ "${MODE}" == "core" ]]; then
        find \
            .clang-format .clang-tidy .gitignore CMakeLists.txt README.md \
            docs include src scripts misc \
            -type f 2>/dev/null
    else
        find . -type f \
            -not -path './.git/*' \
            -not -path './build*' \
            -not -path './out/*' \
            -not -path '*/__pycache__/*' \
            -not -path './.venv/*' \
            -not -path './.cache/*' \
            -not -path './.idea/*' \
            -not -path './.vscode/*'
    fi \
    | sed 's|^\./||' \
    | grep -v -E "^(${OUT_REL}|project_dump.*\.txt)$" \
    | grep -v -E '\.(rknn|onnx|pt|pth|whl|ppm|pgm|pbm|raw|nv12|nv16|rgb|jpg|jpeg|png|bmp|gif|mp4|mkv|ts|nut|h264|bin|o|a|so|dylib|exe|pyc|csv|log|tar|gz|zip)$' \
    | LC_ALL=C sort -u
}

mapfile -t FILES < <(collect_files)

if [[ "${#FILES[@]}" -eq 0 ]]; then
    echo "Error: no files matched in ${ROOT}" >&2
    exit 1
fi

{
    echo "# Project tree"
    printf '%s\n' "${FILES[@]}"
    echo

    for f in "${FILES[@]}"; do
        echo "================================================================================"
        echo "FILE: ${f}"
        echo "================================================================================"
        cat "${f}"
        # Guarantee trailing newline before the next separator
        if [[ -s "${f}" && "$(tail -c1 "${f}" | wc -l)" -eq 0 ]]; then
            echo
        fi
        echo
    done
} > "${OUT}"

LINES="$(wc -l < "${OUT}")"
BYTES="$(wc -c < "${OUT}")"
echo "Saved ${#FILES[@]} files (${LINES} lines, ${BYTES} bytes) -> ${OUT} (mode: ${MODE})"
