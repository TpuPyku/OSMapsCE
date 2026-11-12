"""Yandex Disk's REST API as the maps workflow and the app use it (yadisk.py): asked of a made-up server.

    python -m unittest discover -s offline/test
"""
import io
import json
import os
import sys
import tempfile
import unittest
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import yadisk   # noqa: E402


class FakeDisk:
    """The calls asked, the answers given by (method, path) - a path's answer: (status, JSON or bytes)."""
    def __init__(self, answers):
        self.answers, self.calls = answers, []

    def __call__(self, method, url, headers, body=None):
        u = urllib.parse.urlsplit(url)
        q = dict(urllib.parse.parse_qsl(u.query))
        sent = body.read() if hasattr(body, 'read') else body
        self.calls.append((method, u.netloc + u.path, q, headers, sent))
        status, data = self.answers.get((method, u.path), (404, {'error': 'NotFound'}))
        if callable(status):   # (an answer per call: pages)
            status, data = status()
        return status, json.dumps(data).encode() if isinstance(data, dict) else data


class YaDisk(unittest.TestCase):
    def test_upload(self):
        """A file up (the app's folder - the token's only access, the user 2026-10-30): the upload's link asked
        (overwrite), the file PUT to it; the folders on the way made first (one there already: 409, fine)."""
        fake = FakeDisk({('PUT', '/v1/disk/resources'): (409, {'error': 'DiskPathPointsToExistentDirectoryError'}),
                         ('GET', '/v1/disk/resources/upload'): (200, {'href': 'https://up.example/put/1', 'method': 'PUT'}),
                         ('PUT', '/put/1'): (201, b'')})
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b'ZIP' * 100)
        try:
            yadisk.Disk('tok', fake).upload('app:/maps/a.zip', f.name)
        finally:
            os.remove(f.name)
        made = [c[2]['path'] for c in fake.calls if c[0] == 'PUT' and c[1].endswith('/resources')]
        self.assertEqual(made, ['app:/maps'])
        ask = [c for c in fake.calls if c[1].endswith('/resources/upload')][0]
        self.assertEqual(ask[2], {'path': 'app:/maps/a.zip', 'overwrite': 'true'})
        self.assertEqual(ask[3]['Authorization'], 'OAuth tok')
        put = fake.calls[-1]
        self.assertEqual((put[0], put[1], put[4]), ('PUT', 'up.example/put/1', b'ZIP' * 100))
        self.assertNotIn('Authorization', put[3])                       # (the link is the permission)

    def test_errors_told(self):
        """No token / no room / anything else: an error with the server's words."""
        fake = FakeDisk({('GET', '/v1/disk/resources/upload'): (507, {'error': 'InsufficientStorageError'})})
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b'x')
        try:
            with self.assertRaises(yadisk.DiskError) as e:
                yadisk.Disk('tok', fake).upload('disk:/a.zip', f.name)
        finally:
            os.remove(f.name)
        self.assertIn('507', str(e.exception))
        self.assertIn('InsufficientStorageError', str(e.exception))

    def test_upload_again(self):
        """The connection reset on the way up (the maps workflow 2026-11-02: Krasnoyarsk's zip lost, its whole build
        with it) or the server's 5xx: asked again, a new link each time, after a wait; the third failure told."""
        def flaky(fails):
            fake = FakeDisk({('PUT', '/v1/disk/resources'): (201, {}),
                             ('GET', '/v1/disk/resources/upload'): (200, {'href': 'https://up.example/put/1'}),
                             ('PUT', '/put/1'): (201, b'')})
            left = [fails]

            def send(method, url, headers, body=None):
                if '/put/' in url and left[0]:
                    left[0] -= 1
                    if left[0] % 2:
                        raise ConnectionResetError(104, 'Connection reset by peer')
                    return 500, b'busy'
                return fake(method, url, headers, body)
            return fake, send
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b'ZIP')
        try:
            fake, send = flaky(2)
            waits = []
            yadisk.Disk('tok', send, waits.append).upload('app:/maps/a.zip', f.name)
            self.assertEqual(len([c for c in fake.calls if c[1].endswith('/resources/upload')]), 3)
            self.assertEqual(fake.calls[-1][4], b'ZIP')   # (the file whole the last time too)
            self.assertEqual(waits, [30, 60])
            fake, send = flaky(3)   # (500, reset, 500)
            with self.assertRaises(yadisk.DiskError) as e:
                yadisk.Disk('tok', send, waits.append).upload('app:/maps/a.zip', f.name)
            self.assertIn('500', str(e.exception))
            fake, send = flaky(4)   # (reset, 500, reset; the fourth never asked)
            with self.assertRaises(OSError):
                yadisk.Disk('tok', send, waits.append).upload('app:/maps/a.zip', f.name)
        finally:
            os.remove(f.name)

    def test_upload_link_not_found(self):
        """The upload's link answering 404 (the maps workflow 2026-11-06: Kostroma and Kurgan among dozens that went up
        the same way, each losing its whole build): asked again with a new link; another answer of the link - the file
        too big - told at once."""
        def disk(answers):
            fake = FakeDisk({('PUT', '/v1/disk/resources'): (201, {}),
                             ('GET', '/v1/disk/resources/upload'): (200, {'href': 'https://up.example/put/1'}),
                             ('PUT', '/put/1'): (201, b'')})
            left = list(answers)

            def send(method, url, headers, body=None):
                if '/put/' in url and left:
                    return left.pop(0), b''
                return fake(method, url, headers, body)
            return fake, send
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b'ZIP')
        try:
            fake, send = disk([404])
            waits = []
            yadisk.Disk('tok', send, waits.append).upload('app:/maps/a.zip', f.name)
            self.assertEqual(len([c for c in fake.calls if c[1].endswith('/resources/upload')]), 2)
            self.assertEqual(fake.calls[-1][4], b'ZIP')
            self.assertEqual(waits, [30])
            fake, send = disk([404, 404, 404])
            with self.assertRaises(yadisk.DiskError) as e:
                yadisk.Disk('tok', send, waits.append).upload('app:/maps/a.zip', f.name)
            self.assertIn('404', str(e.exception))
            fake, send = disk([413])
            waits = []
            with self.assertRaises(yadisk.DiskError):
                yadisk.Disk('tok', send, waits.append).upload('app:/maps/a.zip', f.name)
            self.assertEqual(waits, [])
        finally:
            os.remove(f.name)

    def test_publish(self):
        """The folder public: published, its public link read back."""
        fake = FakeDisk({('PUT', '/v1/disk/resources/publish'): (200, {'href': 'x'}),
                         ('GET', '/v1/disk/resources'): (200, {'public_url': 'https://disk.yandex.ru/d/abc'})})
        self.assertEqual(yadisk.Disk('tok', fake).publish('app:/maps'), 'https://disk.yandex.ru/d/abc')
        self.assertEqual(fake.calls[-1][2], {'path': 'app:/maps', 'fields': 'public_url'})

    def test_public_link(self):
        """A file of a public folder: its download link, asked without a token (as OSMapsCE asks it)."""
        fake = FakeDisk({('GET', '/v1/disk/public/resources/download'): (200, {'href': 'https://dl.example/a'})})
        self.assertEqual(yadisk.public_href('https://disk.yandex.ru/d/abc', 'a.zip', fake), 'https://dl.example/a')
        self.assertEqual(fake.calls[0][2], {'public_key': 'https://disk.yandex.ru/d/abc', 'path': '/a.zip'})
        self.assertNotIn('Authorization', fake.calls[0][3])

    def test_public_listing(self):
        """The public folder's zips: {name: {size, date}} (the catalog's disk copies, 2026-11-12), pages of 1000, no
        token; another folder's items (not files) left out."""
        page1 = {'_embedded': {'items': [{'name': 'a.zip', 'type': 'file', 'size': 10,
                                          'modified': '2026-11-05T10:00:00+00:00'},
                                         {'name': 'sub', 'type': 'dir'}], 'total': 3, 'limit': 2, 'offset': 0}}
        page2 = {'_embedded': {'items': [{'name': 'b.zip', 'type': 'file', 'size': 20,
                                          'modified': '2026-10-30T01:00:00+00:00'}], 'total': 3, 'limit': 2,
                               'offset': 2}}
        pages = [page1, page2]
        fake = FakeDisk({('GET', '/v1/disk/public/resources'): (lambda: (200, pages.pop(0)), None)})
        got = yadisk.public_listing('https://disk.yandex.ru/d/abc', fake, limit=2)
        self.assertEqual(got, {'a.zip': {'size': 10, 'date': '2026-11-05'}, 'b.zip': {'size': 20, 'date': '2026-10-30'}})
        self.assertEqual(fake.calls[0][2], {'public_key': 'https://disk.yandex.ru/d/abc', 'limit': '2', 'offset': '0'})
        self.assertEqual(fake.calls[1][2]['offset'], '2')
        self.assertNotIn('Authorization', fake.calls[0][3])


if __name__ == '__main__':
    unittest.main()
