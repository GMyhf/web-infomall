#!/usr/bin/env bash
#
# update.sh — build, load data, and (re)start the replay server.
#
# Web InfoMall writes up to 37 non-empty shards (url_00.idx … url_36.idx) plus
# meta/year_dist/today/title_idx/checkpoint. Every load REWRITES them in place;
# it never creates per-run files. Therefore:
#   • first load          -> full build
#   • adding new dat files -> use --incremental (merges old + new); a bare
#                             partial load would leave the old index inconsistent.
# Individual files are published atomically, but the full index set is not one
# transaction. This script backs up the index directory before an incremental
# load (disable with --no-backup).
#
# Usage:
#   ./update.sh --dat <dat_dir> [options]
#
# Options:
#   --dat DIR        Directory holding dat0, dat1, … (required)
#   --archive DIR    Archive directory            (default: archive)
#   --port N         Server port                  (default: 8088)
#   --files LIST     Comma list of file indices, e.g. 0,1,2
#   --all            Discover and load every datN in --dat DIR
#   --full           Full load; ARCHIVE must be empty
#   --no-backup      Skip backing up the index before an incremental load
#   --no-restart     Load only; do not (re)start the server
#   -h, --help       Show this help
#
# Default mode is incremental when an existing index is found, full otherwise.
# A first full load defaults to dat0. Incremental mode auto-discovers pending datN.

set -euo pipefail

# Resolve repo root = directory containing this script. Relative user-supplied
# paths remain relative to the caller's directory, even though the build runs
# from the repository root.
CALLER_CWD="$(pwd -P)"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# ── Defaults ──────────────────────────────────────────────────
DAT_DIR=""
ARCHIVE="archive"
ARCHIVE_EXPLICIT=0
PORT="8088"
FILE_SEL=()          # extra args passed to load for file selection
FILE_MODE=""
FORCE_FULL=0
DO_BACKUP=1
DO_RESTART=1

usage() { sed -n '2,/^$/p' "$ROOT/update.sh" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dat)        DAT_DIR="${2:?--dat needs a value}"; shift 2 ;;
        --archive)    ARCHIVE="${2:?--archive needs a value}"; ARCHIVE_EXPLICIT=1; shift 2 ;;
        --port)       PORT="${2:?--port needs a value}"; shift 2 ;;
        --files)
            [[ -z "$FILE_MODE" ]] || { echo "ERROR: --files and --all are mutually exclusive" >&2; exit 1; }
            FILE_MODE="files"
            FILE_SEL=(--files "${2:?--files needs a value}")
            shift 2
            ;;
        --all)
            [[ -z "$FILE_MODE" ]] || { echo "ERROR: --files and --all are mutually exclusive" >&2; exit 1; }
            FILE_MODE="all"
            FILE_SEL=(--all)
            shift
            ;;
        --full)       FORCE_FULL=1; shift ;;
        --no-backup)  DO_BACKUP=0; shift ;;
        --no-restart) DO_RESTART=0; shift ;;
        -h|--help)    usage 0 ;;
        *) echo "Unknown option: $1" >&2; usage 1 ;;
    esac
done

[[ -n "$DAT_DIR" ]] || { echo "ERROR: --dat <dat_dir> is required" >&2; usage 1; }
[[ "$DAT_DIR" == /* ]] || DAT_DIR="$CALLER_CWD/$DAT_DIR"
[[ -d "$DAT_DIR" ]] || { echo "ERROR: dat dir not found: $DAT_DIR" >&2; exit 1; }
DAT_DIR="$(cd "$DAT_DIR" && pwd -P)"

if [[ "$ARCHIVE_EXPLICIT" -eq 1 ]]; then
    [[ "$ARCHIVE" == /* ]] || ARCHIVE="$CALLER_CWD/$ARCHIVE"
else
    ARCHIVE="$ROOT/archive"
fi
if [[ -d "$ARCHIVE" ]]; then
    ARCHIVE="$(cd "$ARCHIVE" && pwd -P)"
else
    ARCHIVE_PARENT="$(dirname "$ARCHIVE")"
    [[ -d "$ARCHIVE_PARENT" ]] || {
        echo "ERROR: archive parent directory not found: $ARCHIVE_PARENT" >&2
        exit 1
    }
    ARCHIVE="$(cd "$ARCHIVE_PARENT" && pwd -P)/$(basename "$ARCHIVE")"
fi

if [[ ! "$PORT" =~ ^[0-9]+$ || ${#PORT} -gt 5 ]]; then
    echo "ERROR: --port must be an integer from 1 to 65535" >&2
    exit 1
fi
PORT=$((10#$PORT))
if ((PORT < 1 || PORT > 65535)); then
    echo "ERROR: --port must be an integer from 1 to 65535" >&2
    exit 1
fi
if [[ "$DO_RESTART" -eq 1 ]] && ! command -v curl >/dev/null 2>&1; then
    echo "ERROR: curl is required to health-check the restarted server" >&2
    exit 1
fi

INDEX_DIR="$ARCHIVE/index"
DATA_DIR="$ARCHIVE/data"
PID_FILE="$ARCHIVE/serve.pid"
LOG_FILE="$ARCHIVE/serve.log"

process_matches_archive() {
    local pid="$1"
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1

    if [[ -r "/proc/$pid/cmdline" ]]; then
        local args=() arg
        while IFS= read -r -d '' arg; do
            args+=("$arg")
        done < "/proc/$pid/cmdline"
        [[ ${#args[@]} -ge 3 ]] || return 1
        [[ "${args[0]}" == "./src/serve" || "${args[0]}" == "$ROOT/src/serve" ]] || return 1
        local process_data process_index
        if [[ "${args[1]}" == /* ]]; then
            process_data="$(readlink -f -- "${args[1]}" 2>/dev/null)" || return 1
        else
            process_data="$(readlink -f -- "/proc/$pid/cwd/${args[1]}" 2>/dev/null)" || return 1
        fi
        if [[ "${args[2]}" == /* ]]; then
            process_index="$(readlink -f -- "${args[2]}" 2>/dev/null)" || return 1
        else
            process_index="$(readlink -f -- "/proc/$pid/cwd/${args[2]}" 2>/dev/null)" || return 1
        fi
        [[ "$process_data" == "$DATA_DIR" && "$process_index" == "$INDEX_DIR" ]]
        return
    fi

    # macOS has no /proc; compare the exact command prefix emitted by this script.
    local command_line
    command_line="$(ps -ww -p "$pid" -o command= 2>/dev/null)" || return 1
    [[ "$command_line" == "./src/serve $DATA_DIR $INDEX_DIR" ||
       "$command_line" == "./src/serve $DATA_DIR $INDEX_DIR "* ]]
}

stop_archive_servers() {
    local pids=() seen=" " candidate attempt

    if command -v pgrep >/dev/null 2>&1; then
        while IFS= read -r candidate; do
            if process_matches_archive "$candidate"; then
                pids+=("$candidate")
                seen+="$candidate "
            fi
        done < <(pgrep -x serve 2>/dev/null || true)
    fi

    if [[ -f "$PID_FILE" ]]; then
        IFS= read -r candidate < "$PID_FILE" || true
        if [[ "$seen" != *" $candidate "* ]] && process_matches_archive "$candidate"; then
            pids+=("$candidate")
        fi
    fi

    for candidate in "${pids[@]}"; do
        echo "==> Stopping archive server (pid $candidate)"
        if ! kill "$candidate" 2>/dev/null; then
            echo "ERROR: could not stop server pid $candidate" >&2
            return 1
        fi
        for ((attempt = 0; attempt < 50; attempt++)); do
            kill -0 "$candidate" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$candidate" 2>/dev/null; then
            echo "ERROR: server pid $candidate did not stop within 5 seconds" >&2
            return 1
        fi
    done
    rm -f "$PID_FILE"
}

port_is_listening() {
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") >/dev/null 2>&1
}

# ── 1. Incremental build ───────────────────────────────────────
echo "==> Building binaries (make)…"
make -C src

# ── 2. Decide mode: incremental if an index already exists ────
INCREMENTAL=0
if [[ "$FORCE_FULL" -eq 0 && -d "$INDEX_DIR" ]] && compgen -G "$INDEX_DIR/url_*.idx" >/dev/null; then
    INCREMENTAL=1
fi
if [[ "$FORCE_FULL" -eq 1 ]] && { [[ -d "$DATA_DIR" ]] || [[ -d "$INDEX_DIR" ]]; }; then
    echo "ERROR: --full requires an empty --archive directory: $ARCHIVE" >&2
    echo "       Move the existing archive aside or choose a new path." >&2
    exit 1
fi
if [[ "$INCREMENTAL" -eq 1 && ! -f "$INDEX_DIR/checkpoint.dat" ]]; then
    echo "ERROR: existing archive has no checkpoint and cannot be incremented safely" >&2
    echo "       Rebuild into a new empty archive." >&2
    exit 1
fi

# ── 3. Back up the existing index before an in-place rewrite ──
if [[ "$INCREMENTAL" -eq 1 && "$DO_BACKUP" -eq 1 ]]; then
    BAK="$INDEX_DIR.bak.$(date +%Y%m%d%H%M%S).$$"
    echo "==> Backing up index -> $BAK"
    cp -a "$INDEX_DIR" "$BAK"
fi

# ── 4. Load ───────────────────────────────────────────────────
LOAD_ARGS=("$DAT_DIR" "$ARCHIVE" "${FILE_SEL[@]}")
if [[ "$INCREMENTAL" -eq 1 ]]; then
    LOAD_ARGS+=(--incremental)
    echo "==> Incremental load: ${FILE_SEL[*]:-auto-discover pending datN}"
else
    echo "==> Full load: ${FILE_SEL[*]:-dat0}"
fi

if ! ./src/load "${LOAD_ARGS[@]}"; then
    echo "ERROR: load failed." >&2
    if [[ "$INCREMENTAL" -eq 1 ]]; then
        FAILED_CHECKPOINT="$ARCHIVE/checkpoint.failed.$(date +%Y%m%d%H%M%S).$$.dat"
        if [[ -f "$INDEX_DIR/checkpoint.dat" ]]; then
            if cp -p "$INDEX_DIR/checkpoint.dat" "$FAILED_CHECKPOINT"; then
                echo "       Current tainted checkpoint saved at $FAILED_CHECKPOINT" >&2
            else
                echo "WARNING: could not preserve the current tainted checkpoint" >&2
            fi
        fi
        if [[ "$DO_BACKUP" -eq 1 ]]; then
            echo "       The old index backup is at $BAK." >&2
            echo "       Do not restore its checkpoint.dat: retain the current tainted checkpoint" >&2
            echo "       so appended records cannot be replayed as if the failed load never ran." >&2
        fi
        echo "       Treat the archive as tainted after this failed incremental load." >&2
        echo "       Fully reloading an affected source file requires a full archive rebuild." >&2
    else
        echo "       Discard the incomplete archive before retrying a full load." >&2
    fi
    exit 2
fi

# ── 5. (Re)start the server ───────────────────────────────────
if [[ "$DO_RESTART" -eq 1 ]]; then
    echo "==> Restarting server on port $PORT…"
    stop_archive_servers
    if port_is_listening; then
        echo "ERROR: port $PORT is already used by another process" >&2
        exit 3
    fi

    nohup ./src/serve "$DATA_DIR" "$INDEX_DIR" "$PORT" > "$LOG_FILE" 2>&1 &
    SERVER_PID=$!
    printf '%s\n' "$SERVER_PID" > "$PID_FILE.tmp"
    mv "$PID_FILE.tmp" "$PID_FILE"

    SERVER_READY=0
    for ((attempt = 0; attempt < 60; attempt++)); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            break
        fi
        if curl -fsS -m2 "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1; then
            sleep 0.1
            if kill -0 "$SERVER_PID" 2>/dev/null; then
                SERVER_READY=1
                break
            fi
        fi
        sleep 0.5
    done

    if [[ "$SERVER_READY" -eq 0 ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        rm -f "$PID_FILE"
        echo "ERROR: server failed to start; check $LOG_FILE" >&2
        tail -5 "$LOG_FILE" >&2 || true
        exit 3
    fi

    STATS="$(curl -fsS -m5 "http://127.0.0.1:$PORT/stats" 2>/dev/null || true)"
    echo "==> Server up on :$PORT (pid $SERVER_PID)  $STATS"
fi

echo "==> Done."
