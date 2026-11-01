# Local File SQL

File SQL is disabled by default. Use a dedicated allowed directory and enable it
for the session. Setting the global directory requires an administrative local
connection, following the existing `secure_file_priv` rules.

```sql
SET GLOBAL secure_file_priv = '/absolute/allowed/directory';
SET SESSION enable_file_sql = ON;
SELECT id, name FROM read_csv('/absolute/allowed/directory/input.csv', header=true)
  WHERE id > 1 ORDER BY id;
SELECT * FROM read_json('/absolute/allowed/directory/input.jsonl');
SELECT * FROM read_ndjson('/absolute/allowed/directory/input.jsonl');
SELECT * FROM read_parquet('/absolute/allowed/directory/input.parquet');
DESCRIBE SELECT * FROM read_csv('/absolute/allowed/directory/input.csv');
SELECT file FROM glob('/absolute/allowed/directory/*.csv');
COPY (SELECT * FROM read_csv('/absolute/allowed/directory/input.csv'))
  TO '/absolute/allowed/directory/export.csv' (format csv, header, nullstr 'NULL');
```

CSV options are `header`, `delimiter` / `delim`, `quote`, `escape`, and `nullstr`.
CSV and JSONL accept only `sample_size=-1`: inference reads the complete input
while retaining schema and one record, then rewinds for execution. JSON supports
flat objects, unioned fields, missing values and `format='newline_delimited'`.
Non-null empty strings and values matching `nullstr` are quoted in exported CSV.
`COPY table TO ...` also works. Existing output names are never overwritten.

Readers feed native function-table operators. Projection, expressions, WHERE,
GROUP BY, HAVING, ORDER BY, LIMIT and joins use seekdb's existing SQL execution.
Parquet opens only projected column chunks, retaining at most bounded page and
dictionary buffers. No external query engine is used.

## Supported boundaries

- Linux/POSIX, one local file per reader. Relative paths are relative to the
  authorized directory. Parent traversal and symlink traversal are rejected.
- Full CSV/JSONL schema inference; BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE,
  DATETIME (up to six fractional digits) and all-NULL columns. Integer overflow
  remains text rather than silently losing integer precision.
- Flat Parquet with optional/required primitive columns, multiple row groups,
  data pages V1/V2, PLAIN/dictionary encodings, boolean RLE, uncompressed/Snappy
  compression, dates and timestamps. Nested/repeated data, decimal logical
  types, other encodings/codecs, encrypted files and external chunks fail with
  diagnostics rather than being interpreted incorrectly.
- CSV/JSON records: 16 MiB; columns: 4096; JSON union field names: 1 MiB.
  Encoded plan descriptors: 1 MiB; Parquet footer/dictionaries: 16 MiB; pages:
  64 MiB; retained Parquet buffers and native string results: 128 MiB per scan.
  Glob bounds traversal depth, directory entries and discovered path bytes.
- File identity, size and nanosecond modification/change times are checked
  through open descriptors. Execution/reopening also rechecks schema, identity,
  the session switch and the current global directory.
- COPY streams into an exclusive temporary file in the authorized directory,
  then publishes exclusively after successful execution; failure removes it.

## Verification

Build against the competition branch with the documented seekdb toolchain.
After configuring the normal build, enable the focused unit targets:

```bash
cmake -S . -B build_release -DSEEKDB_FILE_SQL_TESTS=ON
cmake --build build_release --target test_file_scan_utils test_file_sql_parser -j4
ctest --test-dir build_release -R 'file_scan_utils|file_sql_parser' --output-on-failure
```

Native tests require a dedicated running instance and PyMySQL as a test client:

```bash
export FILE_SQL_SOCKET=/path/to/instance/run/sql.sock
export FILE_SQL_TEST_ROOT=/path/to/test-parent
python3 unittest/sql/engine/basic/test_file_sql_integration.py
python3 unittest/sql/engine/basic/run_file_sql_public.py \
  --socket "$FILE_SQL_SOCKET" --test-parent "$FILE_SQL_TEST_ROOT" \
  --log-dir /path/to/test-logs
```

The native suite creates and removes its own temporary files and one uniquely
named database. Both runners temporarily set and restore `secure_file_priv`.
The public runner keeps the public SQL and expected results intact, normalizes
checkout CRLF line endings in a temporary expected file, and replaces only the
distributed SSH deployment/cleanup includes with local fixture deployment.

Parquet fixture regeneration uses PyArrow only as a development-time writer:
`python3 unittest/sql/engine/basic/make_file_sql_fixtures.py`. Neither PyArrow
nor Python is part of the product scan or SQL execution path.
