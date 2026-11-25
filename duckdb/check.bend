# CLI test driver for bend-data-duckdb.
# Modes:
#   f32  <db> <sql>   -> prints shape, column names, rows, and a second-pass sum (tests Data reuse)
#   text <db> <sql>   -> prints shape, column names, and string rows
#   exec <db> <sql>   -> executes DDL/DML and prints OK
import Base
import ./duckdb.bend as D

def join_str(+sep: String, xs: List<&2, String>) -> String:
  match xs:
    case Nil{}:
      ""
    case Con{h, Nil{}}:
      h
    case Con{h, t}:
      h ++ sep ++ join_str(sep, t)

def show_f32_cell.go(is_nan: Bool, x: F32) -> String:
  match is_nan:
    case True{}:
      "NaN"
    case False{}:
      F32.show(x)

def show_f32_cell(+x: F32) -> String:
  show_f32_cell.go(F32.is_ne(x, x), x)

def show_f32_row(xs: List<&2, F32>) -> String:
  match xs:
    case Nil{}:
      ""
    case Con{h, Nil{}}:
      show_f32_cell(h)
    case Con{h, t}:
      show_f32_cell(h) ++ " " ++ show_f32_row(t)

def show_f32_rows(rs: List<&2, List<&2, F32>>) -> String:
  match rs:
    case Nil{}:
      ""
    case Con{r, t}:
      show_f32_row(r) ++ "\n" ++ show_f32_rows(t)

def sum_f32_row.add(is_nan: Bool, h: F32, acc: F32) -> F32:
  match is_nan:
    case True{}:
      acc
    case False{}:
      (acc + h : F32)

def sum_f32_row(xs: List<&2, F32>, acc: F32) -> F32:
  match xs:
    case Nil{}:
      acc
    case Con{+h, t}:
      sum_f32_row(t, sum_f32_row.add(F32.is_ne(h, h), h, acc))

def sum_f32_rows(rs: List<&2, List<&2, F32>>, acc: F32) -> F32:
  match rs:
    case Nil{}:
      acc
    case Con{r, t}:
      sum_f32_rows(t, sum_f32_row(r, acc))

def show_text_rows(rs: List<&2, List<&2, String>>) -> String:
  match rs:
    case Nil{}:
      ""
    case Con{r, t}:
      join_str("|", r) ++ "\n" ++ show_text_rows(t)

def sum_tbl(t: D.F32Table) -> F32:
  D.F32Table{_, _, _, rows} = t
  sum_f32_rows(rows, 0.0)

def render_f32_tbl(+tbl: D.F32Table) -> String:
  # Use tbl twice to verify Data reuse / refcounting across the C effect boundary.
  D.F32Table{nr, nc, names, rows} = tbl
  s = sum_tbl(tbl)
  "F32 " ++ U32.show(nr) ++ " " ++ U32.show(nc) ++ "\n"
    ++ "COLS " ++ join_str("|", names) ++ "\n"
    ++ show_f32_rows(rows)
    ++ "SUM " ++ F32.show(s) ++ "\n"

def render_f32(r: Result<&1, &1, U32 & String, D.F32Table>) -> String:
  match r:
    case Fail{(code, msg)}:
      "FAIL " ++ U32.show(code) ++ " " ++ msg ++ "\n"
    case Done{tbl}:
      render_f32_tbl(tbl)

def render_text(r: Result<&1, &1, U32 & String, D.TextTable>) -> String:
  match r:
    case Fail{(code, msg)}:
      "FAIL " ++ U32.show(code) ++ " " ++ msg ++ "\n"
    case Done{D.TextTable{nr, nc, names, rows}}:
      "TEXT " ++ U32.show(nr) ++ " " ++ U32.show(nc) ++ "\n"
        ++ "COLS " ++ join_str("|", names) ++ "\n"
        ++ show_text_rows(rows)

def render_exec(r: Result<&1, &1, U32 & String, Unit>) -> String:
  match r:
    case Fail{(code, msg)}:
      "FAIL " ++ U32.show(code) ++ " " ++ msg ++ "\n"
    case Done{Unit{}}:
      "OK\n"

def dispatch.text(is_text: Bool, db: String, sql: String) -> IO(Unit):
  match is_text:
    case True{}:
      do IO<Unit>:
        r : Result<&1, &1, U32 & String, D.TextTable> <- D.query_text(db, sql)
        IO.write(render_text(r))
    case False{}:
      do IO<Unit>:
        r : Result<&1, &1, U32 & String, Unit> <- D.exec(db, sql)
        IO.write(render_exec(r))

def dispatch.f32(is_f32: Bool, mode: String, db: String, sql: String) -> IO(Unit):
  match is_f32:
    case True{}:
      do IO<Unit>:
        r : Result<&1, &1, U32 & String, D.F32Table> <- D.query_f32(db, sql)
        IO.write(render_f32(r))
    case False{}:
      dispatch.text(String.eq(mode, "text"), db, sql)

def dispatch(+mode: String, db: String, sql: String) -> IO(Unit):
  dispatch.f32(String.eq(mode, "f32"), mode, db, sql)

def run_args(args: List<String>) -> IO(Unit):
  match args:
    case Con{_, Con{mode, Con{db, Con{sql, _}}}}:
      dispatch(mode, db, sql)
    case _:
      IO.print("usage: check <f32|text|exec> <db_path> <sql>")

def main() -> IO(Unit):
  do IO<Unit>:
    args : List<String> <- IO.args()
    run_args(args)
