#!/usr/bin/env bash
#
# update.sh — build (if needed), load data, and (re)start the replay server.
#
# Web InfoMall ships a fixed set of index files (url_00.idx … url_36.idx plus
# meta/year_dist/today/title_idx/checkpoint). Every load REWRITES them in place;
# it never creates per-run files. Therefore:
#   • first load          -> full build
#   • adding new dat files -> use --incremental (merges old + new); a bare
#                             partial load would leave the old index inconsistent.
# Because the rewrite is in-place and non-atomic, this script backs up the index
# directory before an incremental load (disable with --no-backup).
#
# Usage:
#   ./update.sh --dat <dat_dir> [options]
#
# Options:
#   --dat DIR        Directory holding dat0, dat1, … (required)
#   --archive DIR    Archive directory            (default: archive)
#   --port N         Server port                  (default: 8088)
#   --files LIST     Comma list of file indices, e.g. 0,1,2
#   --all            Load dat0..dat111 (loader caps at 112)
#   --full           Force full (non-incremental) rebuild
#   --no-backup      Skip backing up the index before an incremental load
#   --no-restart     Load only; do not (re)start the server
#   -h, --help       Show this help
#
# Default mode is incremental when an existing index is found, full otherwise.
# If neither --files nor --all is given, the loader defaults to dat0 only.

set -euo pipefail

# Resolve repo root = directory containing this script.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# ── Defaults ──────────────────────────────────────────────────
DAT_DIR=""
ARCHIVE="archive"
PORT="8088"
FILE_SEL=()          # extra args passed to load for file selection
FORCE_FULL=0
DO_BACKUP=1
DO_RESTART=1

usage() { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dat)        DAT_DIR="${2:?--dat needs a value}"; shift 2 ;;
        --archive)    ARCHIVE="${2:?--archive needs a value}"; shift 2 ;;
        --port)       PORT="${2:?--port needs a value}"; shift 2 ;;
        --files)      FILE_SEL=(--files "${2:?--files needs a value}"); shift 2 ;;
        --all)        FILE_SEL=(--all); shift ;;
        --full)       FORCE_FULL=1; shift ;;
        --no-backup)  DO_BACKUP=0; shift ;;
        --no-restart) DO_RESTART=0; shift ;;
        -h|--help)    usage 0 ;;
        *) echo "Unknown option: $1" >&2; usage 1 ;;
    esac
done

[[ -n "$DAT_DIR" ]] || { echo "ERROR: --dat <dat_dir> is required" >&2; usage 1; }
[[ -d "$DAT_DIR" ]] || { echo "ERROR: dat dir not found: $DAT_DIR" >&2; exit 1; }

INDEX_DIR="$ARCHIVE/index"
DATA_DIR="$ARCHIVE/data"

# ── 1. Build binaries if missing ──────────────────────────────
if [[ ! -x src/load || ! -x src/serve ]]; then
    echo "==> Building binaries (make)…"
    make -C src
fi

# ── 2. Decide mode: incremental if an index already exists ────
INCREMENTAL=0
if [[ "$FORCE_FULL" -eq 0 && -d "$INDEX_DIR" ]] && compgen -G "$INDEX_DIR/url_*.idx" >/dev/null; then
    INCREMENTAL=1
fi

# ── 3. Back up the existing index before an in-place rewrite ──
if [[ "$INCREMENTAL" -eq 1 && "$DO_BACKUP" -eq 1 ]]; then
    BAK="$INDEX_DIR.bak"
    echo "==> Backing up index -> $BAK"
    rm -rf "$BAK"
    cp -a "$INDEX_DIR" "$BAK"
fi

# ── 4. Load ───────────────────────────────────────────────────
LOAD_ARGS=("$DAT_DIR" "$ARCHIVE" "${FILE_SEL[@]}")
if [[ "$INCREMENTAL" -eq 1 ]]; then
    LOAD_ARGS+=(--incremental)
    echo "==> Incremental load: ${FILE_SEL[*]:-dat0}"
else
    echo "==> Full load: ${FILE_SEL[*]:-dat0}"
fi

if ! ./src/load "${LOAD_ARGS[@]}"; then
    echo "ERROR: load failed." >&2
    if [[ "$INCREMENTAL" -eq 1 && "$DO_BACKUP" -eq 1 ]]; then
        echo "       Index backup is at $INDEX_DIR.bak — restore with:" >&2
        echo "         rm -rf '$INDEX_DIR' && mv '$INDEX_DIR.bak' '$INDEX_DIR'" >&2
    fi
    exit 2
fi

# ── 5. (Re)start the server ───────────────────────────────────
if [[ "$DO_RESTART" -eq 1 ]]; then
    echo "==> Restarting server on port $PORT…"
    # Match this archive's serve process specifically, so other instances live.
    pkill -f "src/serve $DATA_DIR $INDEX_DIR" 2>/dev/null || true
    sleep 1
    nohup ./src/serve "$DATA_DIR" "$INDEX_DIR" "$PORT" > serve.log 2>&1 &
    sleep 2
    if curl -fsS -m5 "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1; then
        STATS="$(curl -fsS -m5 "http://127.0.0.1:$PORT/stats" 2>/dev/null || true)"
        echo "==> Server up on :$PORT  $STATS"
    else
        echo "WARNING: server did not answer /ping; check serve.log" >&2
        tail -5 serve.log >&2 || true
        exit 3
    fi
fi

echo "==> Done."
