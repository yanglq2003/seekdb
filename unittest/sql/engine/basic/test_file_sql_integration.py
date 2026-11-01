# Copyright (c) 2026 OceanBase.
# SPDX-License-Identifier: Apache-2.0
"""Native seekdb regression tests; run only against a dedicated local instance.

FILE_SQL_SOCKET and FILE_SQL_TEST_ROOT must identify that instance and a writable
test parent on the same machine. PyMySQL is a test client, not a query engine.
"""
import datetime
import os
from pathlib import Path
import tempfile
import unittest
import shutil
import threading
import time

import pymysql


class NativeFileSql(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.connection = pymysql.connect(
            unix_socket=os.environ["FILE_SQL_SOCKET"], user="root", autocommit=True
        )
        cls.temp = tempfile.TemporaryDirectory(dir=os.environ["FILE_SQL_TEST_ROOT"])
        cls.root = Path(cls.temp.name)
        cls.cursor = cls.connection.cursor()
        cls.cursor.execute("select @@global.secure_file_priv")
        cls.previous_root = cls.cursor.fetchone()[0]
        cls.cursor.execute("set global secure_file_priv=%s", (str(cls.root),))
        cls.cursor.execute("set session enable_file_sql=ON")
        (cls.root / "a.csv").write_text(
            'id,name,score,active,day,created,note\n'
            '1,Alice,10.5,true,2024-02-29,2024-02-29T12:30:00.123456,"a\nb"\n'
            '2,Bob,20,false,,,"say ""hello"""\n'
            '3,Carol,,true,2024-03-01,2024-03-01 00:00:00,NULL\n', encoding="utf-8"
        )
        (cls.root / "b.jsonl").write_text(
            '{"id":1,"label":"one"}\n{"id":3,"label":"three","extra":true}\n',
            encoding="utf-8",
        )

    @classmethod
    def tearDownClass(cls):
        cls.cursor.execute("set global secure_file_priv=%s", (cls.previous_root,))
        cls.cursor.close()
        cls.connection.close()
        cls.temp.cleanup()

    def rows(self, query):
        self.cursor.execute(query)
        return self.cursor.fetchall()

    def source(self, name="a.csv", function="read_csv", options=""):
        return f"{function}('{self.root / name}'{options})"

    def test_projection_types_filter_sort_and_limit(self):
        rows = self.rows(f"select * from {self.source()} order by id")
        self.assertEqual(3, len(rows))
        self.assertEqual((1, "Alice", 10.5, 1, datetime.date(2024, 2, 29),
                          datetime.datetime(2024, 2, 29, 12, 30, 0, 123456), "a\nb"), rows[0])
        self.assertEqual(((6, "CAROL"),), self.rows(
            f"select id*2 as n,upper(name) from {self.source()} f where active=true order by n desc limit 1"))

    def test_group_having_and_aggregates(self):
        result = self.rows(f"select active,count(*),sum(id),min(id),max(id),avg(score) "
                           f"from {self.source()} group by active having count(*)>1")
        self.assertEqual((1, 2, 4, 1, 3, 10.5), tuple(result[0]))

    def test_describe_and_named_arguments(self):
        schema = self.rows(f"describe select * from {self.source(options=', header=true, sample_size=-1')}")
        self.assertEqual(["BIGINT", "VARCHAR", "DOUBLE", "BOOLEAN", "DATE", "DATETIME", "VARCHAR"],
                         [row[1] for row in schema])
        self.assertEqual(((3,),), self.rows(f"select count(*) from {self.source(options=',header=true')} f"))

    def test_json_union_missing_and_alias(self):
        source = self.source("b.jsonl", "read_ndjson")
        self.assertEqual(((1, "one", None), (3, "three", 1)),
                         self.rows(f"select * from {source} order by id"))

    def test_inner_left_cross_format_and_rescan(self):
        csv, json = self.source(), self.source("b.jsonl", "read_json")
        self.assertEqual(((1, "one"), (3, "three")), self.rows(
            f"select a.id,b.label from {csv} a inner join {json} b on a.id=b.id order by a.id"))
        self.assertEqual(((1, "one"), (2, None), (3, "three")), self.rows(
            f"select a.id,b.label from {csv} a left join {json} b on a.id=b.id order by a.id"))
        self.assertEqual(((9,),), self.rows(
            f"select /*+ use_nl(a b) */ count(*) from {csv} a cross join {csv} b"))

    def test_glob(self):
        self.assertEqual(((str(self.root / "a.csv"),),),
                         self.rows(f"select * from glob('{self.root}/*.csv') where file like '%/a.csv'"))

    def test_copy_roundtrip_and_existing_target(self):
        target = self.root / "copy.csv"
        self.rows(f"copy (select id,name,score from {self.source()} order by id) "
                  f"to '{target}' (format csv,header,delimiter ';',nullstr 'NULL')")
        exported = self.source("copy.csv", options=",delimiter=';',nullstr='NULL'")
        self.assertEqual(((1, "Alice", 10.5), (2, "Bob", 20.0), (3, "Carol", None)),
                         self.rows(f"select * from {exported} order by id"))
        original = target.read_bytes()
        with self.assertRaises(pymysql.MySQLError):
            self.rows(f"copy (select 1) to '{target}' (header)")
        self.assertEqual(original, target.read_bytes())

    def test_gate_and_paths(self):
        self.cursor.execute("set session enable_file_sql=OFF")
        try:
            with self.assertRaises(pymysql.MySQLError):
                self.rows(f"select * from {self.source()}")
            self.assertEqual(((2,),), self.rows("select 1+1"))
        finally:
            self.cursor.execute("set session enable_file_sql=ON")
        for function in ("read_csv", "read_json", "read_parquet", "glob"):
            with self.subTest(function=function), self.assertRaises(pymysql.MySQLError):
                self.rows(f"select * from {function}('/etc/passwd')")
        (self.root / "escape.csv").symlink_to("/etc/passwd")
        with self.assertRaises(pymysql.MySQLError):
            self.rows(f"select * from {self.source('escape.csv')}")

    def test_malformed_diagnostics_and_ordinary_sql(self):
        for name, data, function in (("bad.csv", 'a,b\n1\n', 'read_csv'),
                                     ("bad.jsonl", '{"a":{"nested":1}}\n', 'read_json')):
            (self.root / name).write_text(data, encoding="utf-8")
            with self.subTest(name=name), self.assertRaises(pymysql.MySQLError):
                self.rows(f"select * from {self.source(name,function)}")
        self.assertEqual((("read_csv", 7),), self.rows("select 'read_csv', 3+4"))

    def test_copy_query_failure_removes_output_and_temporary(self):
        (self.root / "payload.csv").write_text('id,payload\n1,1\n2,invalid-json\n', encoding="utf-8")
        before = set(self.root.iterdir())
        with self.assertRaises(pymysql.MySQLError):
            self.rows(f"copy (select id,cast(payload as json) as payload "
                      f"from {self.source('payload.csv')} order by id) "
                      f"to '{self.root}/failed.csv' (header)")
        self.assertEqual(before, set(self.root.iterdir()))

    def test_copy_set_query_and_case_insensitive_reader(self):
        self.assertEqual(((3,),), self.rows(f"select count(*) from READ_CSV('{self.root}/a.csv')"))
        self.rows(f"copy (select id from {self.source()} union all select 4 order by id desc) "
                  f"to '{self.root}/union.csv' (header)")
        self.assertEqual(((4,), (3,), (2,), (1,)), self.rows(
            f"select * from {self.source('union.csv')}"))

    def test_global_path_change_rejects_reused_query(self):
        query = f"select count(*) from {self.source()}"
        self.assertEqual(((3,),), self.rows(query))
        narrower = self.root / "allowed"; narrower.mkdir()
        self.cursor.execute("set global secure_file_priv=%s", (str(narrower),))
        try:
            with self.assertRaises(pymysql.MySQLError): self.rows(query)
        finally:
            self.cursor.execute("set global secure_file_priv=%s", (str(self.root),))

    def test_null_only_and_integer_boundaries(self):
        (self.root / "limits.csv").write_text(
            'small,large,empty\n-9223372036854775808,9223372036854775808,\n'
            '9223372036854775807,18446744073709551616,\n', encoding="utf-8")
        self.assertEqual(((-9223372036854775808, "9223372036854775808", None),
                          (9223372036854775807, "18446744073709551616", None)),
                         self.rows(f"select * from {self.source('limits.csv')} order by small"))
        self.assertEqual(["BIGINT", "VARCHAR", "NULL"],
                         [r[1] for r in self.rows(f"describe select * from {self.source('limits.csv')}")])

    def test_parquet_pages_projection_and_cross_format_join(self):
        fixtures = Path(os.environ.get("FILE_SQL_FIXTURES", Path(__file__).parent / "file_sql_data"))
        paths = sorted(fixtures.glob("*.parquet"))
        self.assertEqual(8, len(paths))
        for path in paths:
            with self.subTest(path=path.name):
                shutil.copyfile(path, self.root / path.name)
                table = self.source(path.name, "read_parquet")
                self.assertEqual(((7, 21, 0, 6),), self.rows(f"select count(*),sum(id),min(id),max(id) from {table}"))
                self.assertEqual((("zero", datetime.date(2024, 2, 29),
                                   datetime.datetime(2024, 1, 1, 12, 13, 14)),),
                                 self.rows(f"select label,day,created from {table} where id=0"))
                self.assertEqual(((1, "one"), (3, "three")), self.rows(
                    f"select a.id,p.label from {self.source()} a join {table} p on a.id=p.id where a.id<>2 order by a.id"))

    def test_file_change_during_native_query_fails(self):
        path = self.root / "changing.csv"
        path.write_text("id\n" + "\n".join(str(i) for i in range(3000)) + "\n", encoding="utf-8")
        def change():
            time.sleep(0.2)
            with path.open("a", encoding="utf-8") as output: output.write("3000\n")
        thread = threading.Thread(target=change)
        thread.start()
        try:
            with self.assertRaises(pymysql.MySQLError):
                self.rows(f"select sleep(0.001),id from {self.source('changing.csv')}")
        finally:
            thread.join()
        self.assertEqual(((1,),), self.rows("select 1"))

    def test_copy_table_and_large_streaming_scan(self):
        database = "file_sql_" + self.root.name
        self.rows(f"create database `{database}`")
        table = f"`{database}`.export_test"
        try:
            self.rows(f"create table {table}(id bigint, text_value varchar(64))")
            self.rows(f"insert into {table} values (1,'quote \" and comma,'),(2,NULL)")
            self.rows(f"copy {table} to '{self.root}/table.csv' (header,nullstr 'NULL')")
            exported = self.source("table.csv", options=",nullstr='NULL'")
            self.assertEqual(((1, 'quote " and comma,'), (2, None)), self.rows(
                f"select * from {exported} order by id"))
        finally:
            self.rows(f"drop database `{database}`")
        path = self.root / "large.csv"
        with path.open("w", encoding="utf-8") as output:
            output.write("id,value\n")
            for i in range(100000): output.write(f"{i},abcdefgh\n")
        self.assertEqual(((100000, 4999950000),), self.rows(
            f"select count(*),sum(id) from {self.source('large.csv')}"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
