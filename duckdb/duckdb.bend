# Native DuckDB driver for Bend via libduckdb (dlopen).
#
# Public types: F32Table, TextTable.
# Public defs:  query_f32, query_text, exec.
#
# All query_* defs open file paths with access_mode=READ_ONLY (and open
# ":memory:" or "" as an in-memory database, which can also read Parquet,
# CSV, and JSON files via DuckDB table functions).
#
# Numeric results return Bend's reusable `List<&2, List<&2, F32>>` row-major
# layout with SQL NULL mapped to IEEE-754 NaN, matching bend-quant's Panel
# and Mat representation with zero copying.
import Base

# Row-major F32 query result (SQL NULL -> NaN), with column names.
type F32Table is Data:
  F32Table{rows: U32, cols: U32, names: List<&2, String>, data: List<&2, List<&2, F32>>}

# Row-major String query result (SQL NULL -> ""), with column names.
type TextTable is Data:
  TextTable{rows: U32, cols: U32, names: List<&2, String>, data: List<&2, List<&2, String>>}

# ---- Raw runtime effects (internal) ----

def raw.query_f32(db_path: String, sql: String) ->
  IO(Result<&1, &1, U32 & String, U32 & (U32 & (List<&2, String> & List<&2, List<&2, F32>>))>):
  import "./duckdb.c"
  import "./duckdb.js"

def raw.query_text(db_path: String, sql: String) ->
  IO(Result<&1, &1, U32 & String, U32 & (U32 & (List<&2, String> & List<&2, List<&2, String>>))>):
  import "./duckdb.c"
  import "./duckdb.js"

def raw.exec(db_path: String, sql: String) ->
  IO(Result<&1, &1, U32 & String, Unit>):
  import "./duckdb.c"
  import "./duckdb.js"

# ---- Result wrappers (internal) ----

def wrap.f32(r: Result<&1, &1, U32 & String, U32 & (U32 & (List<&2, String> & List<&2, List<&2, F32>>))>) ->
  Result<&1, &1, U32 & String, F32Table>:
  match r:
    case Fail{e}:
      Fail{e}
    case Done{(nrows, (ncols, (names, rows)))}:
      Done{F32Table{nrows, ncols, names, rows}}

def wrap.text(r: Result<&1, &1, U32 & String, U32 & (U32 & (List<&2, String> & List<&2, List<&2, String>>))>) ->
  Result<&1, &1, U32 & String, TextTable>:
  match r:
    case Fail{e}:
      Fail{e}
    case Done{(nrows, (ncols, (names, rows)))}:
      Done{TextTable{nrows, ncols, names, rows}}

# ---- Public API ----

# Executes a SQL query returning numeric columns as a row-major F32Table.
# SQL NULL values become NaN; non-numeric columns fail with EINVAL.
# File databases are opened with access_mode=READ_ONLY (protecting the opened
# database file; DuckDB SQL can still COPY TO or ATTACH external files).
def query_f32(db_path: String, sql: String) -> IO(Result<&1, &1, U32 & String, F32Table>):
  do IO<Result<&1, &1, U32 & String, F32Table>>:
    r : Result<&1, &1, U32 & String, U32 & (U32 & (List<&2, String> & List<&2, List<&2, F32>>))> <- raw.query_f32(db_path, sql)
    return wrap.f32(r)

# Executes a SQL query returning columns formatted as Strings (SQL NULL -> "").
# Types libduckdb can't format (UUID, TIMESTAMPTZ, ENUM, nested, ...) fail with
# EINVAL. Cast them with ::VARCHAR.
# File databases are opened with access_mode=READ_ONLY.
def query_text(db_path: String, sql: String) -> IO(Result<&1, &1, U32 & String, TextTable>):
  do IO<Result<&1, &1, U32 & String, TextTable>>:
    r : Result<&1, &1, U32 & String, U32 & (U32 & (List<&2, String> & List<&2, List<&2, String>>))> <- raw.query_text(db_path, sql)
    return wrap.text(r)

# Executes one or more SQL statements in read-write mode (DDL, INSERT, COPY, etc.).
# Each call opens and closes the database, so ":memory:" state does not persist across calls.
def exec(db_path: String, sql: String) -> IO(Result<&1, &1, U32 & String, Unit>):
  raw.exec(db_path, sql)
