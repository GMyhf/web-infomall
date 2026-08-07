"""Regression tests for the retained Python dataset parser."""

import os
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from unittest import mock

import load_data
from parser import ArticleParser


def record(article_id, title, body, terminated=True):
    fields = [
        f"id={article_id}".encode("ascii"),
        b"time=20030114",
        f"url=http://example.test/{article_id}".encode("ascii"),
        b"title=" + title.encode("gb18030"),
        b"body=" + body.encode("gb18030"),
    ]
    data = ArticleParser.LINE_SEP.join(fields)
    return data + (ArticleParser.RECORD_SEP if terminated else b"")


class ArticleParserTests(unittest.TestCase):
    def parse_bytes(self, data, max_articles=None):
        fd, path = tempfile.mkstemp(prefix="web-infomall-parser-")
        try:
            with os.fdopen(fd, "wb") as output:
                output.write(data)
            parser = ArticleParser()
            return list(parser.parse_file(path, max_articles=max_articles)), parser.errors
        finally:
            os.unlink(path)

    def test_gb18030_and_max_articles(self):
        data = record(1, "中文标题", "正文") + record(2, "第二篇", "正文二")
        articles, errors = self.parse_bytes(data, max_articles=1)
        self.assertEqual([article.article_id for article in articles], [1])
        self.assertEqual(articles[0].title, "中文标题")
        self.assertEqual(errors, [])

    def test_unterminated_final_record_is_discarded(self):
        complete = record(1, "完整", "正文")
        truncated = record(2, "截断", "不完整正文", terminated=False)
        articles, errors = self.parse_bytes(complete + truncated)
        self.assertEqual([article.article_id for article in articles], [1])
        self.assertEqual(len(errors), 1)
        self.assertIn("missing record separator", errors[0])


class FakeStore:
    def __init__(self):
        self.urls = []

    def insert_batch(self, articles, source_file=""):
        self.urls.extend(article[0] for article in articles)


class LoadDataTests(unittest.TestCase):
    def write_source(self, directory, index, data):
        with open(os.path.join(directory, f"dat{index}"), "wb") as output:
            output.write(data)

    def test_max_is_global_across_source_files(self):
        with tempfile.TemporaryDirectory(prefix="web-infomall-load-") as tmp:
            self.write_source(tmp, 0, record(1, "一", "正文"))
            self.write_source(tmp, 1, record(2, "二", "正文") + record(3, "三", "正文"))
            store = FakeStore()
            with mock.patch.object(load_data, "DATA_DIR", tmp), redirect_stdout(StringIO()):
                total, errors = load_data.load_files([0, 1], store, max_total=2)
        self.assertEqual(total, 2)
        self.assertEqual(errors, 0)
        self.assertEqual(store.urls, ["http://example.test/1", "http://example.test/2"])

    def test_parse_errors_are_counted_once_per_file(self):
        with tempfile.TemporaryDirectory(prefix="web-infomall-load-") as tmp:
            for index in (0, 1):
                self.write_source(
                    tmp,
                    index,
                    record(index + 1, "完整", "正文")
                    + record(index + 10, "截断", "正文", terminated=False),
                )
            store = FakeStore()
            with mock.patch.object(load_data, "DATA_DIR", tmp), redirect_stdout(StringIO()):
                total, errors = load_data.load_files([0, 1], store)
        self.assertEqual(total, 2)
        self.assertEqual(errors, 2)


if __name__ == "__main__":
    unittest.main()
