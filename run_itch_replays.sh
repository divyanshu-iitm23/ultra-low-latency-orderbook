#!/usr/bin/env bash


set -uo pipefail

# ---------------- defaults ----------------
BINARY=""
SYMBOLS="AAPL"
DOCS_DIR="docs"
OUTFILE=""
DATA_DIR="$HOME/market-data"

usage() {
    cat <<EOF
Usage: $0 [options] [itch_file ...]

Runs itch_book_replay for each ITCH50 file x each symbol and writes a
timestamped Markdown report into the docs/ directory.

Options:
  -s SYMBOLS   Comma-separated symbols (default: AAPL), e.g. -s AAPL,MSFT,SPY
  -b BINARY    Path to itch_book_replay (default: auto-detect ./ and ./build/)
  -d DOCS_DIR  Output directory (default: docs)
  -o OUTFILE   Explicit output file (default: docs/itch_replay_<timestamp>.md)
  -h           Show this help

If no files are given, all *.NASDAQ_ITCH50 files in $DATA_DIR are used.
Note: put options BEFORE file arguments.
EOF
}

# ---------------- parse options ----------------
while getopts ":s:b:d:o:h" opt; do
    case "$opt" in
        s) SYMBOLS="$OPTARG" ;;
        b) BINARY="$OPTARG" ;;
        d) DOCS_DIR="$OPTARG" ;;
        o) OUTFILE="$OPTARG" ;;
        h) usage; exit 0 ;;
        \?) echo "Unknown option: -$OPTARG" >&2; usage; exit 1 ;;
        :)  echo "Option -$OPTARG requires an argument" >&2; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

# ---------------- locate the binary ----------------
if [[ -z "$BINARY" ]]; then
    for cand in ./itch_book_replay ./build/itch_book_replay build/itch_book_replay; do
        if [[ -x "$cand" ]]; then BINARY="$cand"; break; fi
    done
fi
if [[ -z "$BINARY" || ! -x "$BINARY" ]]; then
    echo "ERROR: itch_book_replay binary not found or not executable." >&2
    echo "       Build it, or pass it with -b /path/to/itch_book_replay" >&2
    exit 1
fi

# ---------------- collect input files ----------------
declare -a FILES
if [[ $# -gt 0 ]]; then
    FILES=("$@")
else
    shopt -s nullglob
    FILES=("$DATA_DIR"/*.NASDAQ_ITCH50)
    shopt -u nullglob
fi
if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "ERROR: no ITCH50 files given and none found in $DATA_DIR" >&2
    echo "       Pass files explicitly: $0 [options] file1 file2 ..." >&2
    exit 1
fi

# ---------------- split symbols (strip spaces, split on comma) ----------------
SYMBOLS="${SYMBOLS// /}"
IFS=',' read -r -a SYMBOL_ARR <<< "$SYMBOLS"

# ---------------- prepare output ----------------
mkdir -p "$DOCS_DIR"
if [[ -z "$OUTFILE" ]]; then
    TS="$(date +%Y-%m-%d_%H%M%S)"
    OUTFILE="$DOCS_DIR/itch_replay_${TS}.md"
fi

# ---------------- header (creates/truncates the file) ----------------
{
    echo "# ITCH Replay Report"
    echo
    echo "- Generated: $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "- Binary:    \`$BINARY\`"
    echo "- Symbols:   ${SYMBOL_ARR[*]}"
    echo "- Files:     ${#FILES[@]}"
    echo
} | tee "$OUTFILE"

# ---------------- run each (file x symbol) ----------------
run_count=0
for f in "${FILES[@]}"; do
    if [[ ! -f "$f" ]]; then
        echo "WARNING: skipping missing file: $f" | tee -a "$OUTFILE"
        continue
    fi
    fsize="$(du -h "$f" 2>/dev/null | cut -f1)"
    for sym in "${SYMBOL_ARR[@]}"; do
        [[ -z "$sym" ]] && continue
        {
            echo "## $(basename "$f") — $sym"
            echo
            echo "- File: \`$f\` ($fsize)"
            echo '```'
        } | tee -a "$OUTFILE"

        start=$SECONDS
        # run binary; merge stdout+stderr; show on terminal AND append to report
        "$BINARY" "$f" "$sym" 2>&1 | tee -a "$OUTFILE"
        rc=${PIPESTATUS[0]}
        elapsed=$((SECONDS - start))

        {
            echo '```'
            echo
            echo "_exit code: ${rc} · elapsed: ${elapsed}s_"
            echo
        } | tee -a "$OUTFILE"
        run_count=$((run_count + 1))
    done
done

echo "Done: ${run_count} run(s). Report saved to ${OUTFILE}" | tee -a "$OUTFILE"