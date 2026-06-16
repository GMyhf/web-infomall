"""
Load articles from TenMillionArticles dataset into the archive store.

Phase 1: Load ~100K articles from dat0 for validation.
Phase 2: Load all 14M+ articles from all dat files.

Usage:
    python3 load_data.py                  # Phase 1: load dat0 only (~118K articles)
    python3 load_data.py --all            # Phase 2: load all files
    python3 load_data.py --files dat0,dat1,dat2  # Load specific files
    python3 load_data.py --max 10000      # Load at most 10K articles from dat0
"""

import argparse
import os
import sys
import time

# Add infomall to path for imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from parser import ArticleParser
from store import ArchiveStore


DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'TenMillionArticles', 'dat')
DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'archive.db')


def format_duration(seconds: float) -> str:
    if seconds < 60:
        return f"{seconds:.1f}s"
    elif seconds < 3600:
        return f"{seconds/60:.1f}m"
    else:
        h = int(seconds / 3600)
        m = int((seconds % 3600) / 60)
        return f"{h}h{m}m"


def load_files(file_indices: list[int], store: ArchiveStore, max_per_file: int = None):
    """Load articles from specified .dat files into the store."""
    parser = ArticleParser()
    total = 0
    total_errors = 0

    for idx in file_indices:
        filepath = os.path.join(DATA_DIR, f'dat{idx}')
        if not os.path.exists(filepath):
            print(f"  [SKIP] {filepath} not found")
            continue

        file_size_mb = os.path.getsize(filepath) / (1024 * 1024)
        print(f"  Loading dat{idx} ({file_size_mb:.0f}MB)...", end=' ', flush=True)

        t0 = time.time()
        count = 0
        batch = []
        batch_size = 5000

        for article in parser.parse_file(filepath, max_articles=max_per_file):
            batch.append((
                article.url,
                article.time,
                article.title,
                article.body,
            ))
            if len(batch) >= batch_size:
                store.insert_batch(batch, source_file=f'dat{idx}')
                count += len(batch)
                batch.clear()

            if max_per_file and count >= max_per_file:
                break

        # Insert remaining
        if batch:
            store.insert_batch(batch, source_file=f'dat{idx}')
            count += len(batch)

        elapsed = time.time() - t0
        rate = count / elapsed if elapsed > 0 else 0
        total += count
        total_errors += len(parser.errors)

        status = "✓" if parser.errors == [] else f"⚠ ({len(parser.errors)} errors)"
        print(f"{status} {count} articles in {format_duration(elapsed)} ({rate:.0f} rec/s)")

        if max_per_file and total >= max_per_file:
            break

    return total, total_errors


def main():
    ap = argparse.ArgumentParser(description="Load articles into the web archive store")
    ap.add_argument('--all', action='store_true', help='Load all 112 dat files')
    ap.add_argument('--files', type=str, default='', help='Comma-separated file indices, e.g. "0,1,2"')
    ap.add_argument('--max', type=int, default=0, help='Max articles to load total')
    ap.add_argument('--db', type=str, default=DB_PATH, help='SQLite database path')
    args = ap.parse_args()

    store = ArchiveStore(args.db)

    # Determine which files to load
    if args.all:
        file_indices = list(range(112))
    elif args.files:
        file_indices = [int(x.strip()) for x in args.files.split(',')]
    else:
        # Phase 1 default: just dat0
        file_indices = [0]

    print(f"Archive Store: {args.db}")
    print(f"Loading {len(file_indices)} file(s): dat{',dat'.join(str(i) for i in file_indices)}")
    print()

    t_start = time.time()
    total, errors = load_files(file_indices, store, max_per_file=args.max)
    t_total = time.time() - t_start

    print()
    print(f"Done! {total} articles loaded in {format_duration(t_total)}")

    # Show stats
    stats = store.get_stats()
    print()
    print("=== Archive Statistics ===")
    print(f"  Total pages:      {stats['total_pages']:,}")
    print(f"  Unique URLs:      {stats['total_unique_urls']:,}")
    print(f"  Unique hosts:     {stats['total_hosts']:,}")
    print(f"  Date range:       {stats['first_date']} — {stats['last_date']}")
    if errors:
        print(f"  Parse errors:     {errors}")

    store.close()


if __name__ == '__main__':
    main()
