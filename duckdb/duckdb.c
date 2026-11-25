// DuckDB native effect for Bend (via dlopen of libduckdb).
//
// Loaded dynamically at runtime so `bend <file> -o <out>` needs no extra
// linker flags. Library search order:
//   1. $DUCKDB_LIB (if set and non-empty, only this path is tried)
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
  BDDB_TYPE_TIMESTAMP = 12,
  BDDB_TYPE_DATE      = 13,
  BDDB_TYPE_TIME      = 14,
  BDDB_TYPE_INTERVAL  = 15,
  BDDB_TYPE_HUGEINT   = 16,
  BDDB_TYPE_VARCHAR   = 17,
  BDDB_TYPE_BLOB      = 18,
  BDDB_TYPE_DECIMAL   = 19,
  BDDB_TYPE_UHUGEINT  = 32,
  BDDB_TYPE_SQLNULL   = 36,
};

typedef void* bddb_database;
typedef void* bddb_connection;
typedef void* bddb_config;
typedef void* bddb_data_chunk;
typedef void* bddb_vector;
typedef void* bddb_logical_type;
typedef void* bddb_value;

typedef struct { int32_t days; }   bddb_date;
typedef struct { int64_t micros; } bddb_time;
typedef struct { int64_t micros; } bddb_timestamp;

typedef struct {
  int32_t months;
  int32_t days;
  int64_t micros;
} bddb_interval;

typedef struct {
  uint64_t lower;
  int64_t  upper;
} bddb_hugeint;

typedef struct {
  uint64_t lower;
  uint64_t upper;
} bddb_uhugeint;

typedef struct {
  uint8_t      width;
  uint8_t      scale;
  bddb_hugeint value;
} bddb_decimal;

typedef struct {
  union {
    struct {
      uint32_t length;
      char     prefix[4];
      char*    ptr;
    } pointer;
    struct {
      uint32_t length;
      char     inlined[12];
    } inlined;
  } value;
} bddb_string_t;

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
  bddb_state        (*open_ext)(const char*, bddb_database*, bddb_config, char**);
  void              (*close)(bddb_database*);
  bddb_state        (*connect)(bddb_database, bddb_connection*);
  void              (*disconnect)(bddb_connection*);
  bddb_state        (*create_config)(bddb_config*);
  bddb_state        (*set_config)(bddb_config, const char*, const char*);
  void              (*destroy_config)(bddb_config*);
  bddb_state        (*query)(bddb_connection, const char*, bddb_result*);
  void              (*destroy_result)(bddb_result*);
  bddb_idx_t        (*column_count)(bddb_result*);
  bddb_idx_t        (*row_count)(bddb_result*);
  const char*       (*column_name)(bddb_result*, bddb_idx_t);
  bddb_type         (*column_type)(bddb_result*, bddb_idx_t);
  bddb_logical_type (*column_logical_type)(bddb_result*, bddb_idx_t);
  void              (*destroy_logical_type)(bddb_logical_type*);
  uint8_t           (*decimal_width)(bddb_logical_type);
  uint8_t           (*decimal_scale)(bddb_logical_type);
  bddb_type         (*decimal_internal_type)(bddb_logical_type);
  const char*       (*result_error)(bddb_result*);
  bool              (*value_is_null)(bddb_result*, bddb_idx_t, bddb_idx_t);
  float             (*value_float)(bddb_result*, bddb_idx_t, bddb_idx_t);
  bddb_idx_t        (*result_chunk_count)(bddb_result);
  bddb_data_chunk   (*result_get_chunk)(bddb_result, bddb_idx_t);
  void              (*destroy_data_chunk)(bddb_data_chunk*);
  bddb_idx_t        (*data_chunk_get_size)(bddb_data_chunk);
  bddb_vector       (*data_chunk_get_vector)(bddb_data_chunk, bddb_idx_t);
  void*             (*vector_get_data)(bddb_vector);
  uint64_t*         (*vector_get_validity)(bddb_vector);
  bddb_value        (*create_float)(float);
  bddb_value        (*create_double)(double);
  bddb_value        (*create_date)(bddb_date);
  bddb_value        (*create_time)(bddb_time);
  bddb_value        (*create_timestamp)(bddb_timestamp);
  bddb_value        (*create_interval)(bddb_interval);
  bddb_value        (*create_hugeint)(bddb_hugeint);
  bddb_value        (*create_uhugeint)(bddb_uhugeint);
  bddb_value        (*create_decimal)(bddb_decimal);
  bddb_value        (*create_blob)(const uint8_t*, bddb_idx_t);
  char*             (*get_varchar)(bddb_value);
  void              (*destroy_value)(bddb_value*);
  void              (*free_fn)(void*);
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

static char* bddb_memdup_str(const char* s, size_t n) {
  char* p = (char*)io_mem(malloc(n + 1));
  if (n > 0) {
    memcpy(p, s, n);
  }
  p[n] = '\0';
  return p;
}

static char* bddb_fmt_type_err(const char* fn, const char* nm, bddb_idx_t col,
  const char* kind, bddb_type ct, const char* hint) {
  int need = snprintf(NULL, 0,
    "DuckDB: %s column '%s' (index %llu) has %s type %d (%s)",
    fn, nm, (unsigned long long)col, kind, ct, hint);
  if (need < 0) {
    return bddb_strdup("DuckDB: unsupported column type");
  }
  char* buf = (char*)io_mem(malloc((size_t)need + 1));
  snprintf(buf, (size_t)need + 1,
    "DuckDB: %s column '%s' (index %llu) has %s type %d (%s)",
    fn, nm, (unsigned long long)col, kind, ct, hint);
  return buf;
}

static bool bddb_is_numeric_type(bddb_type t) {
  return (t >= BDDB_TYPE_TINYINT && t <= BDDB_TYPE_DOUBLE)
      || t == BDDB_TYPE_HUGEINT
      || t == BDDB_TYPE_UHUGEINT
      || t == BDDB_TYPE_DECIMAL
      || t == BDDB_TYPE_SQLNULL;
}

// Types query_text formats (BOOLEAN..DECIMAL, UHUGEINT, NULL).
// Matches the JS lane so both lanes accept/reject the exact same column types.
static bool bddb_is_text_type(bddb_type t) {
  return (t >= BDDB_TYPE_BOOLEAN && t <= BDDB_TYPE_DECIMAL)
      || t == BDDB_TYPE_UHUGEINT
      || t == BDDB_TYPE_SQLNULL;
}

static bool bddb_resolve_syms(void* h, BddbApi* api) {
  memset(api, 0, sizeof *api);
  api->handle                = h;
  api->open_ext              = (bddb_state(*)(const char*, bddb_database*, bddb_config, char**))dlsym(h, "duckdb_open_ext");
  api->close                 = (void(*)(bddb_database*))dlsym(h, "duckdb_close");
  api->connect               = (bddb_state(*)(bddb_database, bddb_connection*))dlsym(h, "duckdb_connect");
  api->disconnect            = (void(*)(bddb_connection*))dlsym(h, "duckdb_disconnect");
  api->create_config         = (bddb_state(*)(bddb_config*))dlsym(h, "duckdb_create_config");
  api->set_config            = (bddb_state(*)(bddb_config, const char*, const char*))dlsym(h, "duckdb_set_config");
  api->destroy_config        = (void(*)(bddb_config*))dlsym(h, "duckdb_destroy_config");
  api->query                 = (bddb_state(*)(bddb_connection, const char*, bddb_result*))dlsym(h, "duckdb_query");
  api->destroy_result        = (void(*)(bddb_result*))dlsym(h, "duckdb_destroy_result");
  api->column_count          = (bddb_idx_t(*)(bddb_result*))dlsym(h, "duckdb_column_count");
  api->row_count             = (bddb_idx_t(*)(bddb_result*))dlsym(h, "duckdb_row_count");
  api->column_name           = (const char*(*)(bddb_result*, bddb_idx_t))dlsym(h, "duckdb_column_name");
  api->column_type           = (bddb_type(*)(bddb_result*, bddb_idx_t))dlsym(h, "duckdb_column_type");
  api->column_logical_type   = (bddb_logical_type(*)(bddb_result*, bddb_idx_t))dlsym(h, "duckdb_column_logical_type");
  api->destroy_logical_type  = (void(*)(bddb_logical_type*))dlsym(h, "duckdb_destroy_logical_type");
  api->decimal_width         = (uint8_t(*)(bddb_logical_type))dlsym(h, "duckdb_decimal_width");
  api->decimal_scale         = (uint8_t(*)(bddb_logical_type))dlsym(h, "duckdb_decimal_scale");
  api->decimal_internal_type = (bddb_type(*)(bddb_logical_type))dlsym(h, "duckdb_decimal_internal_type");
  api->result_error          = (const char*(*)(bddb_result*))dlsym(h, "duckdb_result_error");
  api->value_is_null         = (bool(*)(bddb_result*, bddb_idx_t, bddb_idx_t))dlsym(h, "duckdb_value_is_null");
  api->value_float           = (float(*)(bddb_result*, bddb_idx_t, bddb_idx_t))dlsym(h, "duckdb_value_float");
  api->result_chunk_count    = (bddb_idx_t(*)(bddb_result))dlsym(h, "duckdb_result_chunk_count");
  api->result_get_chunk      = (bddb_data_chunk(*)(bddb_result, bddb_idx_t))dlsym(h, "duckdb_result_get_chunk");
  api->destroy_data_chunk    = (void(*)(bddb_data_chunk*))dlsym(h, "duckdb_destroy_data_chunk");
  api->data_chunk_get_size   = (bddb_idx_t(*)(bddb_data_chunk))dlsym(h, "duckdb_data_chunk_get_size");
  api->data_chunk_get_vector = (bddb_vector(*)(bddb_data_chunk, bddb_idx_t))dlsym(h, "duckdb_data_chunk_get_vector");
  api->vector_get_data       = (void*(*)(bddb_vector))dlsym(h, "duckdb_vector_get_data");
  api->vector_get_validity   = (uint64_t*(*)(bddb_vector))dlsym(h, "duckdb_vector_get_validity");
  api->create_float          = (bddb_value(*)(float))dlsym(h, "duckdb_create_float");
  api->create_double         = (bddb_value(*)(double))dlsym(h, "duckdb_create_double");
  api->create_date           = (bddb_value(*)(bddb_date))dlsym(h, "duckdb_create_date");
  api->create_time           = (bddb_value(*)(bddb_time))dlsym(h, "duckdb_create_time");
  api->create_timestamp      = (bddb_value(*)(bddb_timestamp))dlsym(h, "duckdb_create_timestamp");
  api->create_interval       = (bddb_value(*)(bddb_interval))dlsym(h, "duckdb_create_interval");
  api->create_hugeint        = (bddb_value(*)(bddb_hugeint))dlsym(h, "duckdb_create_hugeint");
  api->create_uhugeint       = (bddb_value(*)(bddb_uhugeint))dlsym(h, "duckdb_create_uhugeint");
  api->create_decimal        = (bddb_value(*)(bddb_decimal))dlsym(h, "duckdb_create_decimal");
  api->create_blob           = (bddb_value(*)(const uint8_t*, bddb_idx_t))dlsym(h, "duckdb_create_blob");
  api->get_varchar           = (char*(*)(bddb_value))dlsym(h, "duckdb_get_varchar");
  api->destroy_value         = (void(*)(bddb_value*))dlsym(h, "duckdb_destroy_value");
  api->free_fn               = (void(*)(void*))dlsym(h, "duckdb_free");

  return api->open_ext && api->close && api->connect && api->disconnect
      && api->create_config && api->set_config && api->destroy_config
      && api->query && api->destroy_result && api->column_count && api->row_count
      && api->column_name && api->column_type && api->column_logical_type
      && api->destroy_logical_type && api->decimal_width && api->decimal_scale
      && api->decimal_internal_type && api->result_error
      && api->value_is_null && api->value_float
      && api->result_chunk_count && api->result_get_chunk && api->destroy_data_chunk
      && api->data_chunk_get_size && api->data_chunk_get_vector
      && api->vector_get_data && api->vector_get_validity
      && api->create_float && api->create_double && api->create_date
      && api->create_time && api->create_timestamp && api->create_interval
      && api->create_hugeint && api->create_uhugeint && api->create_decimal
      && api->create_blob && api->get_varchar && api->destroy_value && api->free_fn;
}

static bool bddb_load_api(u32* code_out, char** err_out) {
  pthread_mutex_lock(&bddb_mu);
  if (bddb_loaded) {
    pthread_mutex_unlock(&bddb_mu);
    return true;
  }

  const char* env_lib = getenv("DUCKDB_LIB");
  if (env_lib != NULL && env_lib[0] != '\0') {
    void* h = dlopen(env_lib, RTLD_LAZY | RTLD_LOCAL);
    if (h == NULL) {
      const char* dl = dlerror();
      int need = snprintf(NULL, 0, "DuckDB: could not load DUCKDB_LIB '%s': %s",
        env_lib, dl != NULL ? dl : "dlopen failed");
      char* msg = (char*)io_mem(malloc((size_t)(need > 0 ? need : 64) + 1));
      snprintf(msg, (size_t)(need > 0 ? need : 64) + 1,
        "DuckDB: could not load DUCKDB_LIB '%s': %s",
        env_lib, dl != NULL ? dl : "dlopen failed");
      *code_out = ENOENT;
      *err_out  = msg;
      pthread_mutex_unlock(&bddb_mu);
      return false;
    }
    BddbApi api;
    if (!bddb_resolve_syms(h, &api)) {
      dlclose(h);
      int need = snprintf(NULL, 0,
        "DuckDB: DUCKDB_LIB '%s' is missing required C API symbols", env_lib);
      char* msg = (char*)io_mem(malloc((size_t)(need > 0 ? need : 64) + 1));
      snprintf(msg, (size_t)(need > 0 ? need : 64) + 1,
        "DuckDB: DUCKDB_LIB '%s' is missing required C API symbols", env_lib);
      *code_out = ENOSYS;
      *err_out  = msg;
      pthread_mutex_unlock(&bddb_mu);
      return false;
    }
    bddb_api    = api;
    bddb_loaded = true;
    pthread_mutex_unlock(&bddb_mu);
    return true;
  }

  const char* candidates[] = {
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

  bool saw_missing_syms = false;
  for (int i = 0; candidates[i] != NULL; i += 1) {
    void* h = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
    if (h == NULL) {
      continue;
    }
    BddbApi api;
    if (bddb_resolve_syms(h, &api)) {
      bddb_api    = api;
      bddb_loaded = true;
      pthread_mutex_unlock(&bddb_mu);
      return true;
    }
    dlclose(h);
    saw_missing_syms = true;
  }

  if (saw_missing_syms) {
    *code_out = ENOSYS;
    *err_out  = bddb_strdup("DuckDB: libduckdb is missing required C API symbols");
  } else {
    *code_out = ENOENT;
    *err_out  = bddb_strdup("DuckDB: could not load libduckdb (install duckdb or set DUCKDB_LIB)");
  }
  pthread_mutex_unlock(&bddb_mu);
  return false;
}

typedef struct {
  bddb_type type;
  uint8_t   dec_width;
  uint8_t   dec_scale;
  bddb_type dec_internal;
} BddbColMeta;

static char* bddb_format_val(bddb_value v, size_t* len_out) {
  if (v == NULL) {
    *len_out = 0;
    return NULL;
  }
  char* s = bddb_api.get_varchar(v);
  bddb_api.destroy_value(&v);
  if (s == NULL) {
    *len_out = 0;
    return NULL;
  }
  size_t len = strlen(s);
  char*  out = bddb_memdup_str(s, len);
  bddb_api.free_fn(s);
  *len_out = len;
  return out;
}

static char* bddb_format_cell(const BddbColMeta* meta, const void* data, bddb_idx_t i,
  size_t* len_out) {
  char numbuf[64];
  switch (meta->type) {
    case BDDB_TYPE_VARCHAR: {
      const bddb_string_t* arr = (const bddb_string_t*)data;
      uint32_t len = arr[i].value.inlined.length;
      const char* bytes = (len <= 12)
        ? arr[i].value.inlined.inlined
        : arr[i].value.pointer.ptr;
      *len_out = (size_t)len;
      return bddb_memdup_str(bytes != NULL ? bytes : "", (size_t)len);
    }
    case BDDB_TYPE_BLOB: {
      const bddb_string_t* arr = (const bddb_string_t*)data;
      uint32_t len = arr[i].value.inlined.length;
      const char* bytes = (len <= 12)
        ? arr[i].value.inlined.inlined
        : arr[i].value.pointer.ptr;
      return bddb_format_val(
        bddb_api.create_blob((const uint8_t*)(bytes != NULL ? bytes : ""), (bddb_idx_t)len),
        len_out);
    }
    case BDDB_TYPE_BOOLEAN: {
      bool b = ((const bool*)data)[i];
      const char* s = b ? "true" : "false";
      *len_out = strlen(s);
      return bddb_memdup_str(s, *len_out);
    }
    case BDDB_TYPE_TINYINT: {
      int n = snprintf(numbuf, sizeof numbuf, "%d", (int)((const int8_t*)data)[i]);
      *len_out = (size_t)n;
      return bddb_memdup_str(numbuf, (size_t)n);
    }
    case BDDB_TYPE_SMALLINT: {
      int n = snprintf(numbuf, sizeof numbuf, "%d", (int)((const int16_t*)data)[i]);
      *len_out = (size_t)n;
      return bddb_memdup_str(numbuf, (size_t)n);
    }
    case BDDB_TYPE_INTEGER: {
      int n = snprintf(numbuf, sizeof numbuf, "%d", (int)((const int32_t*)data)[i]);
      *len_out = (size_t)n;
      return bddb_memdup_str(numbuf, (size_t)n);
    }
    case BDDB_TYPE_BIGINT: {
      int n = snprintf(numbuf, sizeof numbuf, "%lld", (long long)((const int64_t*)data)[i]);
      *len_out = (size_t)n;
      return bddb_memdup_str(numbuf, (size_t)n);
    }
    case BDDB_TYPE_UTINYINT: {
      int n = snprintf(numbuf, sizeof numbuf, "%u", (unsigned)((const uint8_t*)data)[i]);
      *len_out = (size_t)n;
      return bddb_memdup_str(numbuf, (size_t)n);
    }
    case BDDB_TYPE_USMALLINT: {
      int n = snprintf(numbuf, sizeof numbuf, "%u", (unsigned)((const uint16_t*)data)[i]);
      *len_out = (size_t)n;
      return bddb_memdup_str(numbuf, (size_t)n);
    }
    case BDDB_TYPE_UINTEGER: {
      int n = snprintf(numbuf, sizeof numbuf, "%u", (unsigned)((const uint32_t*)data)[i]);
      *len_out = (size_t)n;
      return bddb_memdup_str(numbuf, (size_t)n);
    }
    case BDDB_TYPE_UBIGINT: {
      int n = snprintf(numbuf, sizeof numbuf, "%llu", (unsigned long long)((const uint64_t*)data)[i]);
      *len_out = (size_t)n;
      return bddb_memdup_str(numbuf, (size_t)n);
    }
    case BDDB_TYPE_FLOAT:
      return bddb_format_val(bddb_api.create_float(((const float*)data)[i]), len_out);
    case BDDB_TYPE_DOUBLE:
      return bddb_format_val(bddb_api.create_double(((const double*)data)[i]), len_out);
    case BDDB_TYPE_DATE:
      return bddb_format_val(bddb_api.create_date(((const bddb_date*)data)[i]), len_out);
    case BDDB_TYPE_TIME:
      return bddb_format_val(bddb_api.create_time(((const bddb_time*)data)[i]), len_out);
    case BDDB_TYPE_TIMESTAMP:
      return bddb_format_val(bddb_api.create_timestamp(((const bddb_timestamp*)data)[i]), len_out);
    case BDDB_TYPE_INTERVAL:
      return bddb_format_val(bddb_api.create_interval(((const bddb_interval*)data)[i]), len_out);
    case BDDB_TYPE_HUGEINT:
      return bddb_format_val(bddb_api.create_hugeint(((const bddb_hugeint*)data)[i]), len_out);
    case BDDB_TYPE_UHUGEINT:
      return bddb_format_val(bddb_api.create_uhugeint(((const bddb_uhugeint*)data)[i]), len_out);
    case BDDB_TYPE_DECIMAL: {
      bddb_hugeint h = { 0, 0 };
      if (meta->dec_internal == BDDB_TYPE_SMALLINT) {
        int64_t x = (int64_t)((const int16_t*)data)[i];
        h.lower = (uint64_t)x;
        h.upper = x < 0 ? -1 : 0;
      } else if (meta->dec_internal == BDDB_TYPE_INTEGER) {
        int64_t x = (int64_t)((const int32_t*)data)[i];
        h.lower = (uint64_t)x;
        h.upper = x < 0 ? -1 : 0;
      } else if (meta->dec_internal == BDDB_TYPE_BIGINT) {
        int64_t x = ((const int64_t*)data)[i];
        h.lower = (uint64_t)x;
        h.upper = x < 0 ? -1 : 0;
      } else {
        h = ((const bddb_hugeint*)data)[i];
      }
      bddb_decimal dec = { meta->dec_width, meta->dec_scale, h };
      return bddb_format_val(bddb_api.create_decimal(dec), len_out);
    }
    default:
      *len_out = 0;
      return NULL;
  }
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
  size_t* str_lens;
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
  free(p->str_lens);
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
  if (!bddb_load_api(&p->code, &p->err)) {
    return;
  }

  bddb_config     cfg  = NULL;
  bddb_database   db   = NULL;
  bddb_connection conn = NULL;
  BddbColMeta*    cols = NULL;
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

  if (nc > 0) {
    cols = (BddbColMeta*)io_mem(calloc((size_t)nc, sizeof(BddbColMeta)));
  }

  // Validate column types before extracting cells so incompatible columns fail
  // loudly instead of silently turning into 0.0 or "".
  for (bddb_idx_t c = 0; c < nc; c += 1) {
    bddb_type ct = bddb_api.column_type(&res, c);
    cols[c].type = ct;
    const char* nm = bddb_api.column_name(&res, c);
    if (nm == NULL) {
      nm = "";
    }
    if (p->mode == BDDB_MODE_F32 && !bddb_is_numeric_type(ct)) {
      p->code = EINVAL;
      p->err  = bddb_fmt_type_err("query_f32", nm, c, "non-numeric", ct,
        "cast to DOUBLE or use query_text");
      goto cleanup;
    }
    if (p->mode == BDDB_MODE_TEXT && !bddb_is_text_type(ct)) {
      p->code = EINVAL;
      p->err  = bddb_fmt_type_err("query_text", nm, c, "unsupported", ct,
        "cast with ::VARCHAR");
      goto cleanup;
    }
    if (p->mode == BDDB_MODE_TEXT && ct == BDDB_TYPE_DECIMAL) {
      bddb_logical_type lt = bddb_api.column_logical_type(&res, c);
      if (lt != NULL) {
        cols[c].dec_width    = bddb_api.decimal_width(lt);
        cols[c].dec_scale    = bddb_api.decimal_scale(lt);
        cols[c].dec_internal = bddb_api.decimal_internal_type(lt);
        bddb_api.destroy_logical_type(&lt);
      }
    }
  }

  size_t elem_size = (p->mode == BDDB_MODE_F32)
    ? sizeof(float)
    : (sizeof(char*) > sizeof(size_t) ? sizeof(char*) : sizeof(size_t));
  if (nc > 0 && (u64)nr > (u64)(SIZE_MAX / elem_size) / (u64)nc) {
    p->code = EOVERFLOW;
    p->err  = bddb_strdup("DuckDB: result allocation size overflow");
    goto cleanup;
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
          dst[c] = bddb_api.value_float(&res, c, r);
        }
      }
    }
  } else if (p->mode == BDDB_MODE_TEXT && total > 0) {
    p->str_vals = (char**)io_mem(calloc((size_t)total, sizeof(char*)));
    p->str_lens = (size_t*)io_mem(calloc((size_t)total, sizeof(size_t)));

    // Extract all columns via DuckDB's data-chunk API so VARCHAR preserves
    // embedded NUL bytes with exact byte lengths and chunks aren't mixed with
    // deprecated duckdb_value_* calls.
    bddb_idx_t nchunks = bddb_api.result_chunk_count(res);
    bddb_idx_t row_off = 0;
    for (bddb_idx_t k = 0; k < nchunks; k += 1) {
      bddb_data_chunk chunk = bddb_api.result_get_chunk(res, k);
      if (chunk == NULL) {
        continue;
      }
      bddb_idx_t csize = bddb_api.data_chunk_get_size(chunk);
      for (bddb_idx_t c = 0; c < nc; c += 1) {
        if (cols[c].type == BDDB_TYPE_SQLNULL) {
          continue;
        }
        bddb_vector vec = bddb_api.data_chunk_get_vector(chunk, c);
        const void* data = bddb_api.vector_get_data(vec);
        const uint64_t* validity = bddb_api.vector_get_validity(vec);
        for (bddb_idx_t i = 0; i < csize && (row_off + i) < nr; i += 1) {
          bool valid = (validity == NULL)
            || ((validity[i / 64] & ((uint64_t)1 << (i % 64))) != 0);
          u64 idx = (u64)(row_off + i) * (u64)nc + (u64)c;
          if (!valid) {
            p->str_vals[idx] = NULL;
            p->str_lens[idx] = 0;
          } else {
            size_t len = 0;
            p->str_vals[idx] = bddb_format_cell(&cols[c], data, i, &len);
            p->str_lens[idx] = len;
          }
        }
      }
      row_off += csize;
      bddb_api.destroy_data_chunk(&chunk);
    }
  }

cleanup:
  free(cols);
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
      u64 base = (u64)(r - 1) * (u64)p->ncols;
      for (u32 c = p->ncols; c > 0; c -= 1) {
        const char* s = p->str_vals[base + (c - 1)];
        size_t len    = p->str_lens[base + (c - 1)];
        Term st = s != NULL ? io_str(e, s, len) : io_str(e, "", 0);
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
