# Native DuckDB driver for Bend via libduckdb (dlopen): numeric (F32Table, SQL NULL -> NaN) and text (TextTable) queries that open file databases with access_mode=READ_ONLY, plus read-write SQL execution (exec). Source: https://github.com/robinmarin/bend-data/tree/main/duckdb
import ./duckdb.bend as DuckDB
