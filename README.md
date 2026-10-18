# wayback-dl

Bulk-downloads archived files from the Wayback Machine, in C++20.

Queries the [CDX API](https://github.com/internetarchive/ia-cdx-server) for every
capture under a URL prefix, picks the best capture per file (latest HTTP 200),
and downloads the raw archived bytes via

```
https://web.archive.org/web/<timestamp>id_/<original-url>
```

(the `id_` flag returns the original file, unmodified by the Wayback Machine's
HTML rewriting).

## Example: Habbo release-6 DCRs

```sh
./build/wayback-dl --out ./habbo-dcr
```

That fetches every file under
`http://www.habbohotel.com/dcr/release6_20040906/` (the default `--url`),
preserving the directory structure under `./habbo-dcr/`, and writes
`./habbo-dcr/manifest.tsv`.

## Build

Requires CMake ≥ 3.16, a C++20 compiler, and libcurl development headers
(e.g. `libcurl-devel` / `libcurl4-openssl-dev`).

```sh
cmake -B build
cmake --build build
```

## Configuration

Settings are read from `config.json` (next to the binary / in the working
directory) when present, then overridden by command-line flags:

```
defaults  <  config.json  <  CLI flags
```

Use `--config <file>` to point at a different config file. See
`config.example.json` for all keys:

| key          | meaning                                        |
|--------------|------------------------------------------------|
| `url`        | base URL prefix to archive                     |
| `out`        | output directory                               |
| `jobs`       | parallel downloads                             |
| `limit`      | max captures to fetch from CDX                 |
| `from`/`to`  | capture date range, `YYYYMMDD` (inclusive)     |
| `all`        | download every snapshot, not just the latest   |
| `force`      | re-download even if the file already exists    |
| `dry_run`    | list captures, do not download                 |
| `verbose`    | print request URLs                             |
| `manifest`   | manifest path (default `<out>/manifest.tsv`)   |
| `user_agent` | User-Agent header                              |
| `sleep_ms`   | polite delay before each download              |
| `timeout`    | per-download timeout in seconds                |

## CLI

```
wayback-dl [options]

  --config <file>     Config file (default: ./config.json)
  --url <prefix>      Base URL prefix to archive
  --out <dir>         Output directory (default: ./wayback)
  --jobs <n>          Parallel downloads (default: 4)
  --limit <n>         Max captures from CDX (default: 100000)
  --from/--to         Capture date range YYYYMMDD
  --all               Every snapshot, not just the latest per file
  --force             Re-download existing files
  --dry-run           List only
  --manifest <file>   Manifest path
  --user-agent <s>    User agent
  --sleep-ms <n>      Delay between downloads
  --timeout <sec>     Per-download timeout
  --verbose           Print request URLs
```

## Project layout

```
.
├── CMakeLists.txt        # C++20, links libcurl + pthreads
├── src/main.cpp          # the whole tool: JSON config, CDX client, download pool
├── config.example.json   # copy to config.json and edit
└── README.md
```

`config.json` (and `wayback/` output) are git-ignored; commit only
`config.example.json`.

## Notes

- The default `--url` targets the Habbo release-6 DCR directory
  (`http://www.habbohotel.com/dcr/release6_20040906/`); point `url` at any
  other archived prefix to reuse the tool.
- The CDX `length` field does not always match the actual `id_` delivery
  size, so resume is existence-based; verify with `--force` if in doubt.

## Behaviour

- **Resume-friendly**: a file that already exists is skipped (re-run the same
  command to finish an interrupted batch); use `--force` to re-fetch.
  (The CDX `length` field does not always match the actual `id_` delivery
  size, so size is not used as the skip criterion.)
- **Retries**: each download is attempted up to 3 times with backoff + jitter.
- **Atomic writes**: files are written to `<name>.part` and renamed into place.
- **Selection**: per original URL the latest HTTP 200 capture wins; URLs that
  were only ever captured as 3xx/4xx are skipped with a warning (redirects
  have no fetchable raw bytes).
- **Manifest**: `manifest.tsv` with `result, timestamp, size, original, local`.
- Exit code: `0` all good, `1` some downloads failed, `2` bad usage/config,
  `3` no captures found.
