# bend-data

Data connectors and storage drivers for [Bend](https://bend-lang.com/), built to compose cleanly with `Base` and zero-copy libraries like [bend-quant](https://github.com/robinmarin/bend-quant).

## Packages

| Package | BendHub | Scope | Docs |
|---|---|---|---|
| **[duckdb](duckdb/)** | `bend-data-duckdb@0.1.0.0` | Native DuckDB driver via `libduckdb` (`dlopen`): numeric (`F32Table`, SQL `NULL` → `NaN`) and text (`TextTable`) queries opening file DBs with `access_mode=READ_ONLY`, plus read-write SQL execution (`exec`). Also reads Parquet, CSV, and JSON directly via `:memory:`. | [README](duckdb/README.md) |

## Quick import (from BendHub)

```bend
import bend-data-duckdb@0.1.0.0/duckdb.bend as DuckDB
```

From a local clone, import `./duckdb/duckdb.bend` instead.

## Verification

Requires `bend` 2.0.40+, `libduckdb` (e.g. `brew install duckdb` on macOS), and `python3`:

```sh
python3 duckdb/check.py        # native C lane
python3 duckdb/check.py --js   # native C + JS/Bun lane
```

## License

MIT — see [LICENSE](LICENSE).
