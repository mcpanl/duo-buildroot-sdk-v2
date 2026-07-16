#!/usr/bin/env python3
"""
Scan Sophgo developer material category pages and list IDs that exist (not 404).

URL pattern: https://developer.sophgo.com/site/index/material/{id}/all.html
404 pages have: <title>Not Found (#404)</title>
"""

import argparse
import csv
import re
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

BASE_URL = "https://developer.sophgo.com/site/index/material/{id}/all.html"
TITLE_RE = re.compile(r"<title>([^<]*)</title>", re.IGNORECASE)
NOT_FOUND_TITLE = "Not Found (#404)"


def fetch_page(material_id: int, timeout: float, retries: int = 2) -> tuple[int, str, str]:
    url = BASE_URL.format(id=material_id)
    last_error = "unknown error"
    html = None

    for attempt in range(retries + 1):
        req = urllib.request.Request(
            url,
            headers={"User-Agent": "sophgo-material-scan/1.0"},
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                html = resp.read().decode("utf-8", errors="replace")
            break
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                html = exc.read().decode("utf-8", errors="replace")
                break
            last_error = f"HTTP {exc.code}"
        except Exception as exc:  # noqa: BLE001 - report any network failure
            last_error = str(exc)

        if attempt < retries:
            time.sleep(0.5 * (attempt + 1))

    if html is None:
        return material_id, url, f"ERROR: {last_error}"

    match = TITLE_RE.search(html)
    title = match.group(1).strip() if match else "(no title)"
    return material_id, url, title


def main() -> int:
    parser = argparse.ArgumentParser(description="Scan Sophgo material pages for valid IDs")
    parser.add_argument("--start", type=int, default=1, help="Start ID (inclusive)")
    parser.add_argument("--end", type=int, default=149, help="End ID (inclusive)")
    parser.add_argument(
        "--workers",
        type=int,
        default=4,
        help="Concurrent requests (default: 8)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=15.0,
        help="Request timeout in seconds",
    )
    parser.add_argument(
        "--delay",
        type=float,
        default=0.0,
        help="Delay between requests in seconds (per worker)",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="sophgo_material_valid.csv",
        help="Output CSV file path",
    )
    args = parser.parse_args()

    ids = range(args.start, args.end + 1)
    valid_rows: list[tuple[int, str, str]] = []
    not_found_count = 0
    error_count = 0

    def check_one(material_id: int) -> tuple[int, str, str, str]:
        if args.delay:
            time.sleep(args.delay)
        mid, url, title = fetch_page(material_id, args.timeout)
        if title == NOT_FOUND_TITLE:
            return mid, url, title, "404"
        if title.startswith("ERROR:"):
            return mid, url, title, "error"
        return mid, url, title, "ok"

    print(f"Scanning IDs {args.start}..{args.end} ...", file=sys.stderr)

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(check_one, i): i for i in ids}
        done = 0
        total = len(ids)
        for future in as_completed(futures):
            done += 1
            mid, url, title, status = future.result()
            if status == "ok":
                valid_rows.append((mid, url, title))
                print(f"[FOUND] id={mid}  {url}  title={title!r}")
            elif status == "404":
                not_found_count += 1
            else:
                error_count += 1
                print(f"[ERROR] id={mid}  {url}  {title}", file=sys.stderr)

            if done % 20 == 0 or done == total:
                print(f"Progress: {done}/{total}", file=sys.stderr)

    valid_rows.sort(key=lambda row: row[0])

    with open(args.output, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["id", "url", "title"])
        writer.writerows(valid_rows)

    print(file=sys.stderr)
    print(f"Done. Valid: {len(valid_rows)}, 404: {not_found_count}, errors: {error_count}", file=sys.stderr)
    print(f"Results saved to: {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
