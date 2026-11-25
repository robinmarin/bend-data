// DuckDB native effect for Bend (via dlopen of libduckdb).
//
// Loaded dynamically at runtime so `bend <file> -o <out>` needs no extra
// linker flags. Library search order:
//   1. $DUCKDB_LIB
//   2. Standard system / Homebrew paths for libduckdb.{dylib,so}
#ifndef BEND_DATA_DUCKDB_C
#define BEND_DATA_DUCKDB_C

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t bddb_idx_t;
typedef int      bddb_state;
typedef int      bddb_type;
enum { BDDB_SUCCESS = 0, BDDB_ERROR = 1 };

// Subset of duckdb_type enum values from duckdb.h.
enum {
  BDDB_TYPE_BOOLEAN   = 1,
  BDDB_TYPE_TINYINT   = 2,
  BDDB_TYPE_SMALLINT  = 3,
  BDDB_TYPE_INTEGER   = 4,
  BDDB_TYPE_BIGINT    = 5,
  BDDB_TYPE_UTINYINT  = 6,
  BDDB_TYPE_USMALLINT = 7,
  BDDB_TYPE_UINTEGER  = 8,
  BDDB_TYPE_UBIGINT   = 9,
  BDDB_TYPE_FLOAT     = 10,
  BDDB_TYPE_DOUBLE    = 11,
  BDDB_TYPE_HUGEINT   = 16,
  BDDB_TYPE_DECIMAL   = 19,
  BDDB_TYPE_UHUGEINT  = 32,
  BDDB_TYPE_SQLNULL   = 36,
};

typedef void* bddb_database;
typedef void* bddb_connection;
typedef void* bddb_config;

typedef struct {
  bddb_idx_t deprecated_column_count;
  bddb_idx_t deprecated_row_count;
  bddb_idx_t deprecated_rows_changed;
  void*      deprecated_columns;
  char*      deprecated_error_message;
  void*      internal_data;
} bddb_result;

typedef struct {
  void* handle;
  bddb_state  (*open_ext)(const char*, bddb_database*, bddb_config, char**);
  void        (*close)(bddb_database*);
  bddb_state  (*connect)(bddb_database, bddb_connection*);
  void        (*disconnect)(bddb_connection*);
  bddb_state  (*create_config)(bddb_config*);
  bddb_state  (*set_config)(bddb_config, const char*, const char*);
  void        (*destroy_config)(bddb_config*);
  bddb_state  (*query)(bddb_connection, const char*, bddb_result*);
  void        (*destroy_result)(bddb_result*);
  bddb_idx_t  (*column_count)(bddb_result*);
  bddb_idx_t  (*row_count)(bddb_result*);
  const char* (*column_name)(bddb_result*, bddb_idx_t);
  bddb_type   (*column_type)(bddb_result*, bddb_idx_t);
  const char* (*result_error)(bddb_result*);
  bool        (*value_is_null)(bddb_result*, bddb_idx_t, bddb_idx_t);
  double      (*value_double)(bddb_result*, bddb_idx_t, bddb_idx_t);
  char*       (*value_varchar)(bddb_result*, bddb_idx_t, bddb_idx_t);
  void        (*free_fn)(void*);
} BddbApi;

static pthread_mutex_t bddb_mu     = PTHREAD_MUTEX_INITIALIZER;
static BddbApi         bddb_api    = { 0 };
static bool            bddb_loaded = false;

static char* bddb_strdup(const char* s) {
  if (s == NULL) {
    return NULL;
  }
  size_t n = strlen(s);
  char*  p = (char*)io_mem(malloc(n + 1));
  memcpy(p, s, n + 1);
  return p;
}

static bool bddb_is_numeric_type(bddb_type t) {
  return (t >= BDDB_TYPE_TINYINT && t <= BDDB_TYPE_DOUBLE)
      || t == BDDB_TYPE_HUGEINT
      || t == BDDB_TYPE_UHUGEINT
      || t == BDDB_TYPE_DECIMAL
      || t == BDDB_TYPE_SQLNULL;
}

// Types duckdb_value_varchar formats (BOOLEAN..DECIMAL, UHUGEINT, NULL).
// Others return NULL (silent "") or crash libduckdb (UUID before another column).
static bool bddb_is_text_type(bddb_type t) {
  return (t >= BDDB_TYPE_BOOLEAN && t <= BDDB_TYPE_DECIMAL)
      || t == BDDB_TYPE_UHUGEINT
      || t == BDDB_TYPE_SQLNULL;
}

static bool bddb_load_api(char** err_out) {
  pthread_mutex_lock(&bddb_mu);
  if (bddb_loaded) {
    pthread_mutex_unlock(&bddb_mu);
    return true;
  }
  const char* env_lib = getenv("DUCKDB_LIB");
  const char* candidates[] = {
    env_lib,
#ifdef __APPLE__
    "libduckdb.dylib",
    "/opt/homebrew/lib/libduckdb.dylib",
    "/usr/local/lib/libduckdb.dylib",
#else
    "libduckdb.so",
    "/usr/local/lib/libduckdb.so",
    "/usr/lib/libduckdb.so",
    "/usr/lib/x86_64-linux-gnu/libduckdb.so",
    "/usr/lib/aarch64-linux-gnu/libduckdb.so",
#endif
    NULL
  };
  void* h = NULL;
  for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])) - 1; i += 1) {
    if (candidates[i] == NULL || candidates[i][0] == '\0') {
      continue;
    }
    h = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
    if (h != NULL) {
      break;
    }
  }
  if (h == NULL) {
    *err_out = bddb_strdup("DuckDB: could not load libduckdb (install duckdb or set DUCKDB_LIB)");
    pthread_mutex_unlock(&bddb_mu);
    return false;
  }
  BddbApi api = { 0 };
  api.handle         = h;
  api.open_ext       = (bddb_state(*)(const char*, bddb_database*, bddb_config, char**))dlsym(h, "duckdb_open_ext");
  api.close          = (void(*)(bddb_database*))dlsym(h, "duckdb_close");
  api.connect        = (bddb_state(*)(bddb_database, bddb_connection*))dlsym(h, "duckdb_connect");
  api.disconnect     = (void(*)(bddb_connection*))dlsym(h, "duckdb_disconnect");
  api.create_config  = (bddb_state(*)(bddb_config*))dlsym(h, "duckdb_create_config");
  api.set_config     = (bddb_state(*)(bddb_config, const char*, const char*))dlsym(h, "duckdb_set_config");
  api.destroy_config = (void(*)(bddb_config*))dlsym(h, "duckdb_destroy_config");
  api.query          = (bddb_state(*)(bddb_connection, const char*, bddb_result*))dlsym(h, "duckdb_query");
  api.destroy_result = (void(*)(bddb_result*))dlsym(h, "duckdb_destroy_result");
  api.column_count   = (bddb_idx_t(*)(bddb_result*))dlsym(h, "duckdb_column_count");
  api.row_count      = (bddb_idx_t(*)(bddb_result*))dlsym(h, "duckdb_row_count");
  api.column_name    = (const char*(*)(bddb_result*, bddb_idx_t))dlsym(h, "duckdb_column_name");
  api.column_type    = (bddb_type(*)(bddb_result*, bddb_idx_t))dlsym(h, "duckdb_column_type");
  api.result_error   = (const char*(*)(bddb_result*))dlsym(h, "duckdb_result_error");
  api.value_is_null  = (bool(*)(bddb_result*, bddb_idx_t, bddb_idx_t))dlsym(h, "duckdb_value_is_null");
  api.value_double   = (double(*)(bddb_result*, bddb_idx_t, bddb_idx_t))dlsym(h, "duckdb_value_double");
  api.value_varchar  = (char*(*)(bddb_result*, bddb_idx_t, bddb_idx_t))dlsym(h, "duckdb_value_varchar");
  api.free_fn        = (void(*)(void*))dlsym(h, "duckdb_free");

  if (!api.open_ext || !api.close || !api.connect || !api.disconnect
      || !api.create_config || !api.set_config || !api.destroy_config
      || !api.query || !api.destroy_result || !api.column_count || !api.row_count
      || !api.column_name || !api.column_type || !api.result_error
      || !api.value_is_null || !api.value_double || !api.value_varchar || !api.free_fn) {
    dlclose(h);
    *err_out = bddb_strdup("DuckDB: libduckdb is missing required C API symbols");
    pthread_mutex_unlock(&bddb_mu);
    return false;
  }
  bddb_api    = api;
  bddb_loaded = true;
  pthread_mutex_unlock(&bddb_mu);
  return true;
}

enum { BDDB_MODE_F32 = 0, BDDB_MODE_TEXT = 1, BDDB_MODE_EXEC = 2 };

typedef struct {
  int     mode;
  char*   db_path;
  char*   sql;
  u32     code;
  char*   err;
  u32     nrows;
  u32     ncols;
  char**  col_names;
  float*  f32_vals;
  char**  str_vals;
} BddbCall;

static void bddb_call_free(BddbCall* p) {
  free(p->db_path);
  free(p->sql);
  free(p->err);
  if (p->col_names != NULL) {
    for (u32 c = 0; c < p->ncols; c += 1) {
      free(p->col_names[c]);
    }
    free(p->col_names);
  }
  free(p->f32_vals);
  if (p->str_vals != NULL) {
    u64 total = (u64)p->nrows * (u64)p->ncols;
    for (u64 i = 0; i < total; i += 1) {
      free(p->str_vals[i]);
    }
    free(p->str_vals);
  }
  free(p);
}

static bool bddb_is_memory_path(const char* path) {
  return path == NULL || path[0] == '\0' || strcmp(path, ":memory:") == 0;
}

static void bddb_run_worker(IoWork* w) {
  BddbCall* p = (BddbCall*)w->data;
  if (p->code != 0) {
    return;
  }
  if (!bddb_load_api(&p->err)) {
    p->code = ENOENT;
    return;
  }

  bddb_config     cfg  = NULL;
  bddb_database   db   = NULL;
  bddb_connection conn = NULL;
  bddb_result     res;
  memset(&res, 0, sizeof res);
  bool have_res = false;

  bool read_only = (p->mode != BDDB_MODE_EXEC) && !bddb_is_memory_path(p->db_path);
  if (read_only) {
    if (bddb_api.create_config(&cfg) != BDDB_SUCCESS
        || bddb_api.set_config(cfg, "access_mode", "READ_ONLY") != BDDB_SUCCESS) {
      p->code = EINVAL;
      p->err  = bddb_strdup("DuckDB: failed to configure READ_ONLY mode");
      goto cleanup;
    }
  }

  char* open_err = NULL;
  const char* path_arg = (p->db_path[0] == '\0') ? NULL : p->db_path;
  if (bddb_api.open_ext(path_arg, &db, cfg, &open_err) != BDDB_SUCCESS) {
    p->code = EIO;
    p->err  = bddb_strdup(open_err != NULL ? open_err : "DuckDB: open failed");
    if (open_err != NULL) {
      bddb_api.free_fn(open_err);
    }
    goto cleanup;
  }
  if (bddb_api.connect(db, &conn) != BDDB_SUCCESS) {
    p->code = EIO;
    p->err  = bddb_strdup("DuckDB: connect failed");
    goto cleanup;
  }
  if (bddb_api.query(conn, p->sql, &res) != BDDB_SUCCESS) {
    have_res = true;
    const char* qerr = bddb_api.result_error(&res);
    p->code = EINVAL;
    p->err  = bddb_strdup(qerr != NULL ? qerr : "DuckDB: query failed");
    goto cleanup;
  }
  have_res = true;

  if (p->mode == BDDB_MODE_EXEC) {
    goto cleanup;
  }

  bddb_idx_t nr = bddb_api.row_count(&res);
  bddb_idx_t nc = bddb_api.column_count(&res);
  if (nr > UINT32_MAX || nc > UINT32_MAX) {
    p->code = EOVERFLOW;
    p->err  = bddb_strdup("DuckDB: result dimensions exceed U32");
    goto cleanup;
  }

  // Validate column types before extracting cells so incompatible columns fail
  // loudly instead of silently turning into 0.0 or "".
  for (bddb_idx_t c = 0; c < nc; c += 1) {
    bddb_type ct = bddb_api.column_type(&res, c);
    const char* nm = bddb_api.column_name(&res, c);
    if (nm == NULL) {
      nm = "";
    }
    if (p->mode == BDDB_MODE_F32 && !bddb_is_numeric_type(ct)) {
      char msg[256];
      snprintf(msg, sizeof msg,
        "DuckDB: query_f32 column '%s' (index %llu) has non-numeric type %d (cast to DOUBLE or use query_text)",
        nm, (unsigned long long)c, ct);
      p->code = EINVAL;
      p->err  = bddb_strdup(msg);
      goto cleanup;
    }
    if (p->mode == BDDB_MODE_TEXT && !bddb_is_text_type(ct)) {
      char msg[256];
      snprintf(msg, sizeof msg,
        "DuckDB: query_text column '%s' (index %llu) has unsupported type %d (cast with ::VARCHAR)",
        nm, (unsigned long long)c, ct);
      p->code = EINVAL;
      p->err  = bddb_strdup(msg);
      goto cleanup;
    }
  }

  p->nrows = (u32)nr;
  p->ncols = (u32)nc;

  if (nc > 0) {
    p->col_names = (char**)io_mem(calloc((size_t)nc, sizeof(char*)));
    for (bddb_idx_t c = 0; c < nc; c += 1) {
      const char* nm = bddb_api.column_name(&res, c);
      p->col_names[c] = bddb_strdup(nm != NULL ? nm : "");
    }
  }

  u64 total = (u64)nr * (u64)nc;
  if (p->mode == BDDB_MODE_F32 && total > 0) {
    p->f32_vals = (float*)io_mem(malloc((size_t)total * sizeof(float)));
    for (bddb_idx_t r = 0; r < nr; r += 1) {
      float* dst = p->f32_vals + (u64)r * (u64)nc;
      for (bddb_idx_t c = 0; c < nc; c += 1) {
        if (bddb_api.value_is_null(&res, c, r)) {
          dst[c] = NAN;
        } else {
          dst[c] = (float)bddb_api.value_double(&res, c, r);
        }
      }
    }
  } else if (p->mode == BDDB_MODE_TEXT && total > 0) {
    p->str_vals = (char**)io_mem(calloc((size_t)total, sizeof(char*)));
    for (bddb_idx_t r = 0; r < nr; r += 1) {
      char** dst = p->str_vals + (u64)r * (u64)nc;
      for (bddb_idx_t c = 0; c < nc; c += 1) {
        if (bddb_api.value_is_null(&res, c, r)) {
          dst[c] = NULL;
        } else {
          char* v = bddb_api.value_varchar(&res, c, r);
          dst[c] = v != NULL ? bddb_strdup(v) : NULL;
          if (v != NULL) {
            bddb_api.free_fn(v);
          }
        }
      }
    }
  }

cleanup:
  if (have_res) {
    bddb_api.destroy_result(&res);
  }
  if (conn != NULL) {
    bddb_api.disconnect(&conn);
  }
  if (db != NULL) {
    bddb_api.close(&db);
  }
  if (cfg != NULL) {
    bddb_api.destroy_config(&cfg);
  }
}

static Term bddb_pack_names(Env e, BddbCall* p) {
  Term xs = term_pak(CID(Nil), 0);
  for (u32 c = p->ncols; c > 0; c -= 1) {
    const char* s = p->col_names[c - 1];
    xs = io_node(e, CID(Con), io_str(e, s, strlen(s)), xs);
  }
  return xs;
}

static Term bddb_pack(Env e, IoWork* w) {
  BddbCall* p = (BddbCall*)w->data;
  Term out;
  if (p->code != 0) {
    out = io_fail(e, p->code, p->err);
  } else if (p->mode == BDDB_MODE_EXEC) {
    out = io_done(e, term_pak(CID(Unit), 0));
  } else if (p->mode == BDDB_MODE_F32) {
    Term names = bddb_pack_names(e, p);
    Term rows  = term_pak(CID(Nil), 0);
    for (u32 r = p->nrows; r > 0; r -= 1) {
      Term row = term_pak(CID(Nil), 0);
      const float* src = p->f32_vals + (u64)(r - 1) * (u64)p->ncols;
      for (u32 c = p->ncols; c > 0; c -= 1) {
        row = io_node(e, CID(Con), (Term)f32_rewrap(src[c - 1]), row);
      }
      rows = io_node(e, CID(Con), row, rows);
    }
    Term payload = io_tup(e, (Term)p->nrows,
      io_tup(e, (Term)p->ncols, io_tup(e, names, rows)));
    out = io_done(e, payload);
  } else {
    Term names = bddb_pack_names(e, p);
    Term rows  = term_pak(CID(Nil), 0);
    for (u32 r = p->nrows; r > 0; r -= 1) {
      Term row = term_pak(CID(Nil), 0);
      char** src = p->str_vals + (u64)(r - 1) * (u64)p->ncols;
      for (u32 c = p->ncols; c > 0; c -= 1) {
        const char* s = src[c - 1];
        Term st = s != NULL ? io_str(e, s, strlen(s)) : io_str(e, "", 0);
        row = io_node(e, CID(Con), st, row);
      }
      rows = io_node(e, CID(Con), row, rows);
    }
    Term payload = io_tup(e, (Term)p->nrows,
      io_tup(e, (Term)p->ncols, io_tup(e, names, rows)));
    out = io_done(e, payload);
  }
  bddb_call_free(p);
  return out;
}

static Term bddb_start(Env e, Term* f, IoWork* w, int mode) {
  BddbCall* p = (BddbCall*)io_mem(calloc(1, sizeof(BddbCall)));
  p->mode = mode;
  u64 db_len = 0, sql_len = 0;
  p->db_path = io_cstr(e, f[0], &db_len);
  p->sql     = io_cstr(e, f[1], &sql_len);
  if (io_nul(p->db_path, db_len) || io_nul(p->sql, sql_len)) {
    p->code = EINVAL;
    p->err  = bddb_strdup("DuckDB: NUL byte in path or SQL");
  }
  w->data = (char*)p;
  return p->code != 0 ? bddb_pack(e, w) : io_work(w, bddb_run_worker, bddb_pack);
}

#ifdef CID(raw.query_f32)
Term bddb_query_f32_run(Env e, Term* f, IoWork* w) {
  return bddb_start(e, f, w, BDDB_MODE_F32);
}
static void __attribute__((constructor)) bddb_query_f32_use(void) {
  io_eff(CID(raw.query_f32), bddb_query_f32_run);
}
#endif

#ifdef CID(raw.query_text)
Term bddb_query_text_run(Env e, Term* f, IoWork* w) {
  return bddb_start(e, f, w, BDDB_MODE_TEXT);
}
static void __attribute__((constructor)) bddb_query_text_use(void) {
  io_eff(CID(raw.query_text), bddb_query_text_run);
}
#endif

#ifdef CID(raw.exec)
Term bddb_exec_run(Env e, Term* f, IoWork* w) {
  return bddb_start(e, f, w, BDDB_MODE_EXEC);
}
static void __attribute__((constructor)) bddb_exec_use(void) {
  io_eff(CID(raw.exec), bddb_exec_run);
}
#endif

#endif
