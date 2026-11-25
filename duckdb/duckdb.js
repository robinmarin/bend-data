// DuckDB JS-lane effect for Bend (Bun FFI via bun:ffi).
// Uses the same libduckdb C API and candidate library paths as duckdb.c.
let _bddb = null;

function bddb_is_numeric_type(t) {
  return (t >= 2 && t <= 11) || t === 16 || t === 19 || t === 32 || t === 36;
}

// Types duckdb_value_varchar formats (BOOLEAN..DECIMAL, UHUGEINT, NULL); see duckdb.c.
function bddb_is_text_type(t) {
  return (t >= 1 && t <= 19) || t === 32 || t === 36;
}

function bddb_fail(code, text) {
  return { $: CID(Fail), error: io_tup(code, text) };
}

function bddb_load() {
  if (_bddb !== null) return _bddb;
  if (typeof Bun === "undefined") {
    throw new Error("DuckDB JS effect requires Bun (or compile natively with `bend file.bend -o out`)");
  }
  const { dlopen, FFIType } = require("bun:ffi");
  const candidates = [
    process.env.DUCKDB_LIB,
    ...(process.platform === "darwin"
      ? [
          "libduckdb.dylib",
          "/opt/homebrew/lib/libduckdb.dylib",
          "/usr/local/lib/libduckdb.dylib",
        ]
      : [
          "libduckdb.so",
          "/usr/local/lib/libduckdb.so",
          "/usr/lib/libduckdb.so",
          "/usr/lib/x86_64-linux-gnu/libduckdb.so",
          "/usr/lib/aarch64-linux-gnu/libduckdb.so",
        ]),
  ].filter(Boolean);

  const syms = {
    duckdb_open_ext:       { args: ["ptr", "ptr", "ptr", "ptr"], returns: FFIType.i32 },
    duckdb_close:          { args: ["ptr"], returns: FFIType.void },
    duckdb_connect:        { args: ["ptr", "ptr"], returns: FFIType.i32 },
    duckdb_disconnect:     { args: ["ptr"], returns: FFIType.void },
    duckdb_create_config:  { args: ["ptr"], returns: FFIType.i32 },
    duckdb_set_config:     { args: ["ptr", "ptr", "ptr"], returns: FFIType.i32 },
    duckdb_destroy_config: { args: ["ptr"], returns: FFIType.void },
    duckdb_query:          { args: ["ptr", "ptr", "ptr"], returns: FFIType.i32 },
    duckdb_destroy_result: { args: ["ptr"], returns: FFIType.void },
    duckdb_column_count:   { args: ["ptr"], returns: FFIType.u64 },
    duckdb_row_count:      { args: ["ptr"], returns: FFIType.u64 },
    duckdb_column_name:    { args: ["ptr", "u64"], returns: FFIType.cstring },
    duckdb_column_type:    { args: ["ptr", "u64"], returns: FFIType.i32 },
    duckdb_result_error:   { args: ["ptr"], returns: FFIType.cstring },
    duckdb_value_is_null:  { args: ["ptr", "u64", "u64"], returns: FFIType.bool },
    duckdb_value_double:   { args: ["ptr", "u64", "u64"], returns: FFIType.f64 },
    duckdb_value_varchar:  { args: ["ptr", "u64", "u64"], returns: FFIType.ptr },
    duckdb_free:           { args: ["ptr"], returns: FFIType.void },
  };

  for (const path of candidates) {
    try {
      _bddb = dlopen(path, syms).symbols;
      return _bddb;
    } catch (_) {}
  }
  throw new Error("DuckDB: could not load libduckdb (install duckdb or set DUCKDB_LIB)");
}

function bddb_cstr(s) {
  return Buffer.from(s + "\0", "utf8");
}

function bddb_list(arr) {
  let xs = { $: CID(Nil) };
  for (let i = arr.length - 1; i >= 0; i--) {
    xs = { $: CID(Con), head: arr[i], tail: xs };
  }
  return xs;
}

function bddb_run(mode, db_path, sql) {
  if (db_path.includes("\0") || sql.includes("\0")) {
    return bddb_fail(22, "DuckDB: NUL byte in path or SQL");
  }
  let api;
  try {
    api = bddb_load();
  } catch (e) {
    return bddb_fail(2, String(e.message || e));
  }
  const { ptr, CString } = require("bun:ffi");
  const cfgBuf  = new BigUint64Array(1);
  const dbBuf   = new BigUint64Array(1);
  const connBuf = new BigUint64Array(1);
  const errBuf  = new BigUint64Array(1);
  const resBuf  = new BigUint64Array(6); // sizeof(duckdb_result) = 48 bytes
  let haveCfg = false, haveDb = false, haveConn = false, haveRes = false;

  try {
    const isMem = db_path === "" || db_path === ":memory:";
    if (mode !== "exec" && !isMem) {
      if (api.duckdb_create_config(ptr(cfgBuf)) !== 0) {
        return bddb_fail(22, "DuckDB: create_config failed");
      }
      haveCfg = true;
      const k = bddb_cstr("access_mode");
      const v = bddb_cstr("READ_ONLY");
      if (api.duckdb_set_config(Number(cfgBuf[0]), ptr(k), ptr(v)) !== 0) {
        return bddb_fail(22, "DuckDB: set_config READ_ONLY failed");
      }
    }
    const pBuf = db_path === "" ? null : bddb_cstr(db_path);
    const rcOpen = api.duckdb_open_ext(
      pBuf ? ptr(pBuf) : 0,
      ptr(dbBuf),
      haveCfg ? Number(cfgBuf[0]) : 0,
      ptr(errBuf)
    );
    if (rcOpen !== 0) {
      let msg = "DuckDB: open failed";
      if (errBuf[0] !== 0n) {
        msg = String(new CString(Number(errBuf[0])));
        api.duckdb_free(Number(errBuf[0]));
      }
      return bddb_fail(5, msg);
    }
    haveDb = true;
    if (api.duckdb_connect(Number(dbBuf[0]), ptr(connBuf)) !== 0) {
      return bddb_fail(5, "DuckDB: connect failed");
    }
    haveConn = true;
    const sBuf = bddb_cstr(sql);
    const rcQ = api.duckdb_query(Number(connBuf[0]), ptr(sBuf), ptr(resBuf));
    haveRes = true;
    if (rcQ !== 0) {
      const msg = String(api.duckdb_result_error(ptr(resBuf)) || "DuckDB: query failed");
      return bddb_fail(22, msg);
    }
    if (mode === "exec") {
      return io_done({ $: CID(Unit) });
    }
    const nrows = Number(api.duckdb_row_count(ptr(resBuf)));
    const ncols = Number(api.duckdb_column_count(ptr(resBuf)));
    const names = [];
    for (let c = 0; c < ncols; c++) {
      const nm = String(api.duckdb_column_name(ptr(resBuf), BigInt(c)) || "");
      const ct = api.duckdb_column_type(ptr(resBuf), BigInt(c));
      if (mode === "f32" && !bddb_is_numeric_type(ct)) {
        return bddb_fail(
          22,
          `DuckDB: query_f32 column '${nm}' (index ${c}) has non-numeric type ${ct} (cast to DOUBLE or use query_text)`
        );
      }
      if (mode === "text" && !bddb_is_text_type(ct)) {
        return bddb_fail(
          22,
          `DuckDB: query_text column '${nm}' (index ${c}) has unsupported type ${ct} (cast with ::VARCHAR)`
        );
      }
      names.push(nm);
    }
    const rows = [];
    for (let r = 0; r < nrows; r++) {
      const row = [];
      for (let c = 0; c < ncols; c++) {
        const isNull = api.duckdb_value_is_null(ptr(resBuf), BigInt(c), BigInt(r));
        if (mode === "f32") {
          row.push(isNull ? NaN : Math.fround(api.duckdb_value_double(ptr(resBuf), BigInt(c), BigInt(r))));
        } else {
          if (isNull) {
            row.push("");
          } else {
            const vp = api.duckdb_value_varchar(ptr(resBuf), BigInt(c), BigInt(r));
            if (vp) {
              row.push(String(new CString(vp)));
              api.duckdb_free(vp);
            } else {
              row.push("");
            }
          }
        }
      }
      rows.push(bddb_list(row));
    }
    return io_done(io_tup(nrows, io_tup(ncols, io_tup(bddb_list(names), bddb_list(rows)))));
  } finally {
    if (haveRes) api.duckdb_destroy_result(ptr(resBuf));
    if (haveConn) api.duckdb_disconnect(ptr(connBuf));
    if (haveDb) api.duckdb_close(ptr(dbBuf));
    if (haveCfg) api.duckdb_destroy_config(ptr(cfgBuf));
  }
}

io_eff(CID(raw.query_f32), (db, sql) => bddb_run("f32", db, sql));
io_eff(CID(raw.query_text), (db, sql) => bddb_run("text", db, sql));
io_eff(CID(raw.exec), (db, sql) => bddb_run("exec", db, sql));
