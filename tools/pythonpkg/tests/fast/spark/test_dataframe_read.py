import pytest

_ = pytest.importorskip("pyduckdb.spark")


class TestSparkSession(object):
    def test_read_csv_basic(self, spark):
        pass
        # spark.read.csv()
