#!/usr/bin/env python3
"""Verification suite for bend-data-duckdb (C native + optional JS/Bun lane)."""
import math
import os
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
BEND = (
    os.environ.get("BEND")
    or shutil.which("bend")
    or os.path.expanduser("~/.bend/bin/bend")
)
BIN = os.path.join(ROOT, "out", "check")


def build():
    os.makedirs(os.path.join(ROOT, "out"), exist_ok=True)
    res = subprocess.run([BEND, "check.bend", "-o", "out/check"], cwd=ROOT, capture_output=True, text=True)
    if res.returncode != 0:
        print("BUILD FAILED:\n", res.stdout, res.stderr)
        sys.exit(1)


def run_cli(mode, db, sql, js=False, env_extra=None):
    cmd = [BEND, "check.bend", "--", mode, db, sql] if js else [BIN, mode, db, sql]
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, env=env)
    if res.returncode != 0:
        raise RuntimeError(f"CLI failed ({res.returncode}): {res.stderr}\n{res.stdout}")
    return res.stdout.splitlines()


def run_suite(js=False):
    tag = "JS" if js else "C"
    with tempfile.TemporaryDirectory() as td:
        db = os.path.join(td, "test.duckdb")

        # 1. In-memory query with mixed numeric types + NULL -> NaN + Data reuse
        lines = run_cli(
            "f32",
            ":memory:",
            "SELECT 1::INTEGER AS a, 2.5::DOUBLE AS b, NULL::DOUBLE AS c "
            "UNION ALL SELECT -3, 0.25, 4.0",
            js=js,
        )
        assert lines[0] == "F32 2 3", lines
        assert lines[1] == "COLS a|b|c", lines
        r0 = lines[2].split()
        r1 = lines[3].split()
        assert math.isclose(float(r0[0]), 1.0, rel_tol=1e-6), r0
        assert math.isclose(float(r0[1]), 2.5, rel_tol=1e-6), r0
        assert r0[2] == "NaN", r0
        assert math.isclose(float(r1[0]), -3.0, rel_tol=1e-6), r1
        assert math.isclose(float(r1[1]), 0.25, rel_tol=1e-6), r1
        assert math.isclose(float(r1[2]), 4.0, rel_tol=1e-6), r1
        assert lines[4].startswith("SUM ") and math.isclose(float(lines[4].split()[1]), 4.75, rel_tol=1e-6), lines

        # 2. exec: create table + insert rows on a real DuckDB file
        lines = run_cli(
            "exec",
            db,
            "CREATE TABLE prices (dt DATE, sym VARCHAR, ret DOUBLE); "
            "INSERT INTO prices VALUES "
            "('2025-01-01', 'BTC', 0.05), "
            "('2025-01-01', 'ETH', -0.02), "
            "('2025-01-02', 'BTC', NULL), "
            "('2025-01-02', 'ETH', 0.03);",
            js=js,
        )
        assert lines == ["OK"], lines

        # 3. query_f32 on file DB with PIVOT (real panel workflow)
        lines = run_cli(
            "f32",
            db,
            "SELECT BTC, ETH FROM (PIVOT prices ON sym USING FIRST(ret) GROUP BY dt ORDER BY dt)",
            js=js,
        )
        assert lines[0] == "F32 2 2", lines
        assert lines[1] == "COLS BTC|ETH", lines
        p0 = lines[2].split()
        p1 = lines[3].split()
        assert math.isclose(float(p0[0]), 0.05, abs_tol=1e-6), p0
        assert math.isclose(float(p0[1]), -0.02, abs_tol=1e-6), p0
        assert p1[0] == "NaN", p1
        assert math.isclose(float(p1[1]), 0.03, abs_tol=1e-6), p1
        assert math.isclose(float(lines[4].split()[1]), 0.06, abs_tol=1e-6), lines

        # 4. query_text with dates, strings, NULL -> ""
        lines = run_cli(
            "text",
            db,
            "SELECT dt::TEXT AS dt, sym, CASE WHEN ret IS NULL THEN NULL ELSE 'ok' END AS st "
            "FROM prices ORDER BY dt, sym",
            js=js,
        )
        assert lines[0] == "TEXT 4 3", lines
        assert lines[1] == "COLS dt|sym|st", lines
        assert lines[2] == "2025-01-01|BTC|ok", lines
        assert lines[4] == "2025-01-02|BTC|", lines

        # 5. Empty 0-row result
        lines = run_cli("f32", db, "SELECT ret FROM prices WHERE 1 = 0", js=js)
        assert lines[0] == "F32 0 1", lines
        assert lines[1] == "COLS ret", lines
        assert lines[2].startswith("SUM ") and float(lines[2].split()[1]) == 0.0, lines

        # 6. Read-only enforcement on the opened database file
        lines = run_cli("f32", db, "DROP TABLE prices", js=js)
        assert lines[0].startswith("FAIL ") and "read-only" in lines[0].lower(), lines

        # 7. SQL syntax error returns Fail cleanly
        lines = run_cli("f32", ":memory:", "SELECT FROM WHERE", js=js)
        assert lines[0].startswith("FAIL "), lines

        # 8. Non-existent DB file returns Fail cleanly
        lines = run_cli("f32", os.path.join(td, "missing.duckdb"), "SELECT 1", js=js)
        assert lines[0].startswith("FAIL "), lines

        # 9. Larger panel (250 x 40 = 10,000 floats) to test scale & speed
        lines = run_cli(
            "f32",
            ":memory:",
            "SELECT " + ", ".join(f"(i + {c})::FLOAT AS c{c}" for c in range(40))
            + " FROM range(250) t(i)",
            js=js,
        )
        assert lines[0] == "F32 250 40", lines[0]
        expected_sum = sum(i + c for i in range(250) for c in range(40))
        got_sum = float(lines[-1].split()[1])
        assert math.isclose(got_sum, expected_sum, rel_tol=1e-5), (got_sum, expected_sum)

        # 10. Direct Parquet read via :memory:
        pq = os.path.join(td, "sample.parquet")
        lines = run_cli("exec", db, f"COPY prices TO '{pq}' (FORMAT PARQUET)", js=js)
        assert lines == ["OK"], lines
        lines = run_cli(
            "f32",
            ":memory:",
            f"SELECT COUNT(*)::DOUBLE AS n FROM read_parquet('{pq}')",
            js=js,
        )
        assert lines[0] == "F32 1 1", lines
        assert math.isclose(float(lines[2]), 4.0, abs_tol=1e-6), lines

        # 11. query_f32 rejects non-numeric columns instead of silently returning 0
        for bad_sql in [
            "SELECT 'abc' AS s",
            "SELECT TIMESTAMP '2025-01-01 00:00:00' AS ts",
            "SELECT DATE '2025-01-01' AS d",
            "SELECT TRUE AS b",
            "SELECT [1, 2] AS xs",
        ]:
            lines = run_cli("f32", ":memory:", bad_sql, js=js)
            assert lines[0].startswith("FAIL 22 ") and "non-numeric" in lines[0], (bad_sql, lines)

        # Untyped NULL literal still maps cleanly to NaN
        lines = run_cli("f32", ":memory:", "SELECT 1.0::DOUBLE AS a, NULL AS b", js=js)
        assert lines[0] == "F32 1 2" and lines[2].split()[1] == "NaN", lines

        # 12. query_text rejects types duckdb_value_string/varchar can't format
        #     (silent "" or, for UUID before another column, a libduckdb segfault)
        uuid = "'00000000-0000-0000-0000-000000000001'::UUID"
        for bad_sql in [
            "SELECT [1, 2] AS xs",
            "SELECT {'a': 1} AS st",
            "SELECT MAP {'k': 1} AS mp",
            f"SELECT {uuid} AS u, 1 AS b",
            "SELECT TIMESTAMPTZ '2025-01-01 00:00:00+00' AS tz",
            "SELECT '2025-01-01'::TIMESTAMP_MS AS ts",
            "SELECT 'a'::ENUM('a', 'b') AS e",
        ]:
            lines = run_cli("text", ":memory:", bad_sql, js=js)
            assert lines[0].startswith("FAIL 22 ") and "unsupported type" in lines[0], (bad_sql, lines)

        # Casting with ::VARCHAR works cleanly
        lines = run_cli(
            "text",
            ":memory:",
            f"SELECT [1, 2]::VARCHAR AS xs, {{'a': 1}}::VARCHAR AS st, {uuid}::VARCHAR AS u, 1 AS b",
            js=js,
        )
        assert lines[0] == "TEXT 1 4", lines
        assert lines[2] == "[1, 2]|{'a': 1}|00000000-0000-0000-0000-000000000001|1", lines

        # Supported scalar types still format identically on C and JS (all DECIMAL widths + BLOB + negative values)
        lines = run_cli(
            "text",
            ":memory:",
            "SELECT TRUE AS b, FALSE AS bf, -170141183460469231731687303715884105727::HUGEINT AS h, "
            "TIME '12:00' AS t, INTERVAL 1 DAY AS iv, "
            "-1.25::DECIMAL(4,2) AS d4, -12345.67::DECIMAL(9,2) AS d9, "
            "-123456789012.34::DECIMAL(18,2) AS d18, -123456789012345678901.23::DECIMAL(38,2) AS d38, "
            "5::UHUGEINT AS uh, '\\x00\\xAB'::BLOB AS bl, DATE '2025-01-01' AS dt, "
            "TIMESTAMP '2025-01-01 01:02:03' AS ts",
            js=js,
        )
        assert lines[2] == (
            "true|false|-170141183460469231731687303715884105727|12:00:00|1 day|"
            "-1.25|-12345.67|-123456789012.34|-123456789012345678901.23|5|\\x00\\xAB|"
            "2025-01-01|2025-01-01 01:02:03"
        ), lines

        # 13. Single-rounding F32 accuracy (no double rounding via double)
        lines = run_cli(
            "f32bits",
            ":memory:",
            "SELECT (1152921710765277183::BIGINT)::FLOAT AS direct_f, "
            "1152921710765277183::BIGINT AS raw_bigint",
            js=js,
        )
        assert lines[0] == "F32BITS 1 2", lines
        b0, b1 = [int(x) for x in lines[2].split()]
        assert b0 == 0x5D800001 and b1 == 0x5D800001, (hex(b0), hex(b1))

        # 14. Long column name (>260 chars) in type error is not truncated
        long_col = "c" * 280
        lines = run_cli("f32", ":memory:", f'SELECT \'x\' AS "{long_col}"', js=js)
        assert lines[0].startswith("FAIL 22 ") and long_col in lines[0] and "use query_text" in lines[0], lines

        # 15. SQL starting with `--` comment works on both C and JS lanes
        lines = run_cli("f32", ":memory:", "-- leading comment\nSELECT 7 AS a", js=js)
        assert lines[0] == "F32 1 1" and float(lines[2]) == 7.0, lines

        # 16. DUCKDB_LIB override fails loudly instead of silently falling back
        lines = run_cli(
            "f32",
            ":memory:",
            "SELECT 1",
            js=js,
            env_extra={"DUCKDB_LIB": os.path.join(td, "does_not_exist.dylib")},
        )
        assert lines[0].startswith("FAIL 2 ") and "DUCKDB_LIB" in lines[0], lines

        if platform.system() == "Darwin":
            enosys = 78
            bad_lib = "/usr/lib/libSystem.B.dylib"
            lines = run_cli(
                "f32",
                ":memory:",
                "SELECT 1",
                js=js,
                env_extra={"DUCKDB_LIB": bad_lib},
            )
            assert lines[0].startswith(f"FAIL {enosys} ") and "missing required C API symbols" in lines[0], lines

        # 17. Embedded NUL bytes in VARCHAR (inlined <=12B, pointer >12B, leading NUL, multi-chunk >2048 rows)
        if not js:
            lines = run_cli(
                "textlen",
                ":memory:",
                "SELECT 'abc' || chr(0) || 'def' AS mid, chr(0) || 'abc' AS lead, "
                "repeat('x', 15) || chr(0) || 'yz' AS long_nul, '' AS emp, NULL::VARCHAR AS nul",
                js=False,
            )
            assert lines[0] == "TEXTLEN 1 5", lines
            assert lines[2] == "7 4 18 0 0", lines

            # Multi-chunk (>2048 rows) mixed VARCHAR + INT with embedded NULs
            lines = run_cli(
                "textlen",
                ":memory:",
                "SELECT i, 'a' || chr(0) || 'b' AS s FROM range(2100) t(i)",
                js=False,
            )
            assert lines[0] == "TEXTLEN 2100 2", lines[0]
            assert lines[2] == "1 3" and lines[-1] == "4 3", (lines[2], lines[-1])

    print(f"[{tag}] All {16 if js else 17} DuckDB checks passed!")


if __name__ == "__main__":
    build()
    run_suite(js=False)
    if "--js" in sys.argv:
        run_suite(js=True)
