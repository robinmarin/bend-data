# duckdb

Native [DuckDB](https://duckdb.org/) driver for [Bend](https://bend-lang.com/) via `libduckdb` (`dlopen`), with zero extra compiler or linker flags.

- **File DB protection on queries:** `query_f32` and `query_text` open file database paths with `access_mode=READ_ONLY` so concurrent readers never lock or modify that database file. Passing `":memory:"` (or `""`) opens an in-memory instance, which can also query Parquet, CSV, and JSON files directly (`read_parquet(...)`, `read_csv(...)`, `read_json(...)`). Note that `READ_ONLY` only protects the opened database file—DuckDB SQL can still write external files via `COPY ... TO` or `ATTACH` another database.
- **Zero-copy `bend-quant` compatibility:** `query_f32` returns `F32Table{rows, cols, names, data}` where `data` is `List<&2, List<&2, F32>>` (SQL `NULL` mapped to IEEE-754 `NaN`), matching `bend-quant-signal`'s `Panel` and `bend-quant-linalg`'s `Mat` row representation directly.
- **Strict column type checks.** `query_f32` fails with `EINVAL` on a non-numeric column instead of returning `0.0`. `query_text` fails with `EINVAL` on any column type `libduckdb` can't format, instead of returning `""`. Cast those columns with `::VARCHAR`.
- **Both C and Bun/JS lanes:** `duckdb.c` runs queries on Bend's worker thread (`io_work`); `duckdb.js` calls the same `libduckdb` C API through `bun:ffi`.

```bend
import Base
import bend-data-duckdb@0.1.0.0/duckdb.bend as D
```

From a clone, import `./duckdb/duckdb.bend` instead.

## Requirements

`libduckdb` must be installed on the system (or pointed to via `DUCKDB_LIB`):

```sh
# macOS (Homebrew)
brew install duckdb

# Custom path override (macOS / Linux)
export DUCKDB_LIB=/path/to/libduckdb.so
```

## Example

Query a pivoted numeric panel directly out of DuckDB (or Parquet via `":memory:"`):

```bend
import Base
import bend-data-duckdb@0.1.0.0/duckdb.bend as D

def summary(r: Result<&1, &1, U32 & String, D.F32Table>) -> String:
  match r:
    case Done{D.F32Table{rows, cols, _, _}}:
      "loaded " ++ U32.show(rows) ++ " x " ++ U32.show(cols) ++ " F32Table"
    case Fail{(_, msg)}:
      "query failed: " ++ msg

def main() -> IO(Unit):
  do IO<Unit>:
    r : Result<&1, &1, U32 & String, D.F32Table> <- D.query_f32(
      ":memory:",
      "SELECT 1.0::DOUBLE AS a, NULL::DOUBLE AS b UNION ALL SELECT 2.5, 3.0"
    )
    IO.print(summary(r))
```

Compose directly with `bend-quant-signal` / `bend-quant-linalg`:

```bend
D.F32Table{t, n, names, rows} = tbl
panel = S.Panel{t, n, rows}   # zero-copy Panel for signal.bend
mat   = L.Mat{t, n, rows}     # zero-copy Mat for linalg/risk/portfolio
```

## API

### Types (`Data`)

- `D.F32Table{rows: U32, cols: U32, names: List<&2, String>, data: List<&2, List<&2, F32>>}`
- `D.TextTable{rows: U32, cols: U32, names: List<&2, String>, data: List<&2, List<&2, String>>}`

### Functions

| def | result | notes |
|---|---|---|
| `D.query_f32(db_path, sql)` | `IO(Result<&1, &1, U32 & String, D.F32Table>)` | Numeric columns as `F32`, `NULL` → `NaN`. Fails (`EINVAL`) on non-numeric columns. Opens file DBs with `access_mode=READ_ONLY`; `":memory:"` / `""` opens in-memory. |
| `D.query_text(db_path, sql)` | `IO(Result<&1, &1, U32 & String, D.TextTable>)` | Columns formatted as `String`, `NULL` → `""`. Fails (`EINVAL`) on types it can't format; cast those with `::VARCHAR`. Opens file DBs with `access_mode=READ_ONLY`. |
| `D.exec(db_path, sql)` | `IO(Result<&1, &1, U32 & String, Unit>)` | Executes one or more SQL statements in read-write mode (DDL, `INSERT`, `COPY`, etc.). |

## Notes & Limits

- **Connection per call:** Every call opens and closes the database inside the effect. That prevents lingering file locks, but means each call pays the cost of opening the database and `exec(":memory:", ...)` does not persist tables across calls (use a file database or put multi-statement SQL in one call).
- **`READ_ONLY` scope:** Opening a file database with `access_mode=READ_ONLY` protects that database file from writes, not the wider filesystem—DuckDB SQL can still run `COPY ... TO` or `ATTACH` another database file.
- **`F32` integer precision:** `query_f32` accepts DuckDB numeric types (`TINYINT`..`UBIGINT`, `HUGEINT`, `UHUGEINT`, `FLOAT`, `DOUBLE`, `DECIMAL`, plus untyped `NULL`) and casts to IEEE-754 `F32`. Because `F32` has 24 bits of significand (~7 decimal digits), integers outside `[-16,777,216, 16,777,216]` (such as 64-bit IDs or epoch timestamps) lose precision—use `query_text` or subtract an offset in SQL when exact large integers are needed.
- **Types `query_text` accepts.** `BOOLEAN`, all integer types, `FLOAT`, `DOUBLE`, `DECIMAL`, `TIMESTAMP`, `DATE`, `TIME`, `INTERVAL`, `VARCHAR`, and `BLOB`. Every other type fails with `EINVAL`, including `TIMESTAMPTZ`, `TIMESTAMP_S`/`_MS`/`_NS`, `TIMETZ`, `UUID`, `ENUM`, `BIT`, `VARINT`, and the nested types. The deprecated `duckdb_value_varchar` returns NULL for those types, and in libduckdb 1.5.2 it segfaults on a `UUID` column followed by another column. Cast them with `::VARCHAR` in SQL.
- **DuckDB C API version:** v0.1 uses `duckdb_row_count` and `duckdb_value_*` (currently deprecated in `duckdb.h`); migrating to DuckDB's data-chunk API is planned for a future release before `libduckdb` removes those symbols.
- **Bend runtime ABI:** Like all native Bend effects, `duckdb.c` targets the current Bend 2 runtime internals and should be recompiled when upgrading `bend`.

## Checks

```sh
python3 duckdb/check.py        # C native lane (12 tests)
python3 duckdb/check.py --js   # C native + JS/Bun lane
```
