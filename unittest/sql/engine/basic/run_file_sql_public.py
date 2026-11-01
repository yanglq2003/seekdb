# Copyright (c) 2026 OceanBase.
# SPDX-License-Identifier: Apache-2.0
"""Run the public File SQL mysqltest against a dedicated local seekdb instance.

The SQL test and expected rows are unchanged. The distributed SSH fixture
deployment includes are replaced by local file deployment for this harness.
"""
import argparse
import base64
from pathlib import Path
import shutil
import subprocess
import tempfile

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--test-parent", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--mysqltest")
    arguments = parser.parse_args()
    repository = Path(__file__).resolve().parents[4]
    public = repository / "tools/deploy/mysql_test/test_suite/file_sql"
    executable = arguments.mysqltest or repository / "deps/3rd/u01/obclient/bin/mysqltest"
    log = Path(arguments.log_dir).resolve()
    log.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="file-sql-public-", dir=arguments.test_parent) as temporary:
        root = Path(temporary).resolve()
        data = root / "data_for_mysqltest_file_sql"
        data.mkdir()
        for name in ("basic.csv", "basic.jsonl", "invalid.txt"):
            shutil.copyfile(public / "data" / name, data / name)
        (data / "simple.parquet").write_bytes(base64.b64decode((public / "data/simple.parquet.b64").read_bytes()))
        include = root / "mysql_test/test_suite/file_sql/include"
        include.mkdir(parents=True)
        for name in ("copy_file_sql_data.inc", "remove_file_sql_data.inc"):
            (include / name).write_text("", encoding="utf-8")
        expected = root / "expected.result"
        expected.write_bytes((public / "r/mysql/file_sql_basic.result").read_bytes().replace(b"\r\n", b"\n"))
        wrapper = root / "run.test"
        wrapper.write_text(f"let $OBSERVER_DIR = {root};\n--source {public}/t/file_sql_basic.test\n", encoding="utf-8")
        connection = pymysql.connect(unix_socket=arguments.socket, user="root", autocommit=True)
        cursor = connection.cursor()
        cursor.execute("select @@global.secure_file_priv")
        previous = cursor.fetchone()[0]
        try:
            cursor.execute("set global secure_file_priv=%s", (str(root),))
            result = subprocess.run([
                str(executable), "--no-defaults", "--user=root", f"--socket={arguments.socket}",
                f"--test-file={wrapper}", f"--result-file={expected}", f"--logdir={log}",
            ], cwd=root, check=False)
            return result.returncode
        finally:
            cursor.execute("set global secure_file_priv=%s", (previous,))
            cursor.close()
            connection.close()


if __name__ == "__main__":
    raise SystemExit(main())
