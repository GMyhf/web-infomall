#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/web-infomall-load-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

archive="$tmp_dir/archive"
source_dir="$tmp_dir/source"
mkdir "$source_dir"
cp ../sample_data/dat0 "$source_dir/dat0"

if ./load "$source_dir" "$tmp_dir/missing-archive" --files 9 \
        >"$tmp_dir/missing.log" 2>&1; then
    echo "FAIL: missing explicit source produced a successful load" >&2
    exit 1
fi
grep -q "Selected source file is missing" "$tmp_dir/missing.log"

: >"$source_dir/dat3"
if ./load "$source_dir" "$tmp_dir/empty-archive" --files 3 \
        >"$tmp_dir/empty.log" 2>&1; then
    echo "FAIL: empty source produced a successful archive" >&2
    exit 1
fi
grep -q "No valid articles were loaded" "$tmp_dir/empty.log"
if find "$tmp_dir/empty-archive/index" -name 'url_*.idx' -print -quit | grep -q .; then
    echo "FAIL: empty source published a shard" >&2
    exit 1
fi

printf 'garbage\037' >"$source_dir/dat4"
if ./load "$source_dir" "$tmp_dir/malformed-archive" --files 4 \
        >"$tmp_dir/malformed.log" 2>&1; then
    echo "FAIL: malformed source produced a successful archive" >&2
    exit 1
fi
grep -q "read/parse error" "$tmp_dir/malformed.log"
grep -q "No valid articles were loaded" "$tmp_dir/malformed.log"
rm -f "$source_dir/dat3" "$source_dir/dat4"

./load "$source_dir" "$archive" --files 0 --max 1 >"$tmp_dir/initial.log" 2>&1
data_file="$archive/data/200301/data_0001.dat"
test -f "$data_file"
size_before=$(wc -c <"$data_file")
grep -q "1 tainted source" "$tmp_dir/initial.log"

if ./load "$source_dir" "$archive" --incremental --files 0 --max 1 \
        >"$tmp_dir/explicit.log" 2>&1; then
    echo "FAIL: explicit replay of a tainted source succeeded" >&2
    exit 1
fi
grep -q "Refusing to replay tainted source" "$tmp_dir/explicit.log"
grep -q "full rebuild" "$tmp_dir/explicit.log"

./load "$source_dir" "$archive" --incremental >"$tmp_dir/automatic.log" 2>&1
grep -q "Skipped 1 tainted source" "$tmp_dir/automatic.log"
size_after=$(wc -c <"$data_file")
if [ "$size_before" != "$size_after" ]; then
    echo "FAIL: automatic incremental load replayed a tainted source" >&2
    exit 1
fi

cp "$source_dir/dat0" "$source_dir/dat2"
dd if=/dev/zero bs=1048576 count=13 >>"$source_dir/dat2" 2>/dev/null
error_archive="$tmp_dir/error-archive"
if ./load "$source_dir" "$error_archive" --files 2 >"$tmp_dir/parse-error.log" 2>&1; then
    echo "FAIL: parser-error source reported a successful load" >&2
    exit 1
fi
grep -q "encountered a read/parse error" "$tmp_dir/parse-error.log"
grep -q "1 tainted source" "$tmp_dir/parse-error.log"
if ./load "$source_dir" "$error_archive" --incremental --files 2 \
        >"$tmp_dir/parse-replay.log" 2>&1; then
    echo "FAIL: parser-error source was replayed" >&2
    exit 1
fi
grep -q "Refusing to replay tainted source" "$tmp_dir/parse-replay.log"

clean_archive="$tmp_dir/clean-archive"
./load "$source_dir" "$clean_archive" --files 0 >"$tmp_dir/clean.log" 2>&1
cp "$source_dir/dat0" "$source_dir/dat1"

unknown_baseline="$tmp_dir/unknown-baseline"
cp -R "$clean_archive" "$unknown_baseline"
rm "$unknown_baseline/index/checkpoint.dat"
find "$unknown_baseline/data" -type f -exec cksum {} \; | sort >"$tmp_dir/unknown.before"
if ./load "$source_dir" "$unknown_baseline" --incremental --files 1 \
        >"$tmp_dir/unknown.log" 2>&1; then
    echo "FAIL: archive without checkpoint accepted an incremental load" >&2
    exit 1
fi
grep -q "baseline is unknown" "$tmp_dir/unknown.log"
find "$unknown_baseline/data" -type f -exec cksum {} \; | sort >"$tmp_dir/unknown.after"
cmp "$tmp_dir/unknown.before" "$tmp_dir/unknown.after"

cp "$clean_archive/index/checkpoint.dat" "$tmp_dir/checkpoint.v3"
# CP2!: one completed entry, dat0. Match the native endian convention used by
# the existing checkpoint format.
case $(od -An -tx1 -N1 "$tmp_dir/checkpoint.v3" | tr -d ' ') in
    21) printf '\041\062\120\103\001\000\000\000\000\000\000\000' ;;
    43) printf '\103\120\062\041\000\000\000\001\000\000\000\000' ;;
    *) echo "FAIL: cannot determine checkpoint byte order" >&2; exit 1 ;;
esac >"$clean_archive/index/checkpoint.dat"
./load "$source_dir" "$clean_archive" --incremental --files 0 \
    >"$tmp_dir/cp2.log" 2>&1
grep -q "Checkpoint: 1 completed, 0 tainted" "$tmp_dir/cp2.log"
mv "$tmp_dir/checkpoint.v3" "$clean_archive/index/checkpoint.dat"

find "$clean_archive/data" -type f -exec cksum {} \; | sort >"$tmp_dir/data.before"

cp "$clean_archive/index/year_dist.dat" "$tmp_dir/year_dist.good"
printf '\377\377\377\377' >"$clean_archive/index/year_dist.dat"
if ./load "$source_dir" "$clean_archive" --incremental --files 1 \
        >"$tmp_dir/bad-aux.log" 2>&1; then
    echo "FAIL: corrupt auxiliary index was accepted" >&2
    exit 1
fi
grep -q "auxiliary indexes are incomplete or invalid" "$tmp_dir/bad-aux.log"
find "$clean_archive/data" -type f -exec cksum {} \; | sort >"$tmp_dir/data.after-aux"
cmp "$tmp_dir/data.before" "$tmp_dir/data.after-aux"

mv "$tmp_dir/year_dist.good" "$clean_archive/index/year_dist.dat"
printf '\000' >"$clean_archive/index/meta.dat"
if ./load "$source_dir" "$clean_archive" --incremental --files 1 \
        >"$tmp_dir/bad-meta.log" 2>&1; then
    echo "FAIL: corrupt archive metadata was accepted" >&2
    exit 1
fi
grep -q "meta.dat does not match" "$tmp_dir/bad-meta.log"
find "$clean_archive/data" -type f -exec cksum {} \; | sort >"$tmp_dir/data.after-meta"
cmp "$tmp_dir/data.before" "$tmp_dir/data.after-meta"

echo "PASS: loader checkpoint and incremental validation regressions"
