# Copyright (c) 2026 OceanBase.
# SPDX-License-Identifier: Apache-2.0
"""Generate small Parquet reader fixtures. PyArrow is a development-only writer.

The product reader has no Python or PyArrow dependency and does not delegate SQL.
"""
from datetime import date, datetime
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

destination = Path(__file__).parent / "file_sql_data"
destination.mkdir(exist_ok=True)
table = pa.table({
    "id": pa.array([0, 1, 2, 3, 4, 5, 6], type=pa.int64()),
    "label": pa.array(["zero", "one", None, "three", "four", "five", "six"]),
    "active": pa.array([True, False, None, True, False, True, True]),
    "score": pa.array([0.5, None, 2.25, 3.5, 4.0, 5.5, 6.0], type=pa.float64()),
    "day": pa.array([date(2024, 2, 29), None, date(1970, 1, 1), date(2000, 1, 1), None, None, None], type=pa.date32()),
    "created": pa.array([datetime(2024, 1, 1, 12, 13, 14), None, None, None, None, None, None], type=pa.timestamp("us")),
})
for page in ("1.0", "2.0"):
    for compression in ("NONE", "SNAPPY"):
        for dictionary in (False, True):
            name = f"flat-v{page}-{compression.lower()}-dict{int(dictionary)}.parquet"
            pq.write_table(table, destination / name, row_group_size=3,
                           data_page_version=page, compression=compression,
                           use_dictionary=dictionary)
