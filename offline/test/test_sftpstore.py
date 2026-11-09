"""The maps' own server (sftpstore.py): its uploads as the sftp commands it sends - asked of a made-up sftp.

    python -m unittest discover -s offline/test
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import sftpstore   # noqa: E402


class FakeSftp:
    """The commands and the batches asked; the exit codes given in turn (then 0)."""
    def __init__(self, codes=()):
        self.codes, self.calls = list(codes), []

    def __call__(self, cmd, batch):
        self.calls.append((cmd, batch))
        return self.codes.pop(0) if self.codes else 0


class SftpStore(unittest.TestCase):
    def store(self, fake):
        slept = []
        s = sftpstore.Store('203.0.113.5', '2222', '/k/key', '/k/hosts', run=fake, sleep=slept.append)
        return s, slept

    def test_upload(self):
        """A file up under its name (the folder's path before it dropped: the chroot's folder is the maps' one) - as
        name.part first, then renamed over the old one (a download meanwhile gets the old one whole); the key, the
        port and the server's known key given, no questions asked."""
        fake = FakeSftp()
        s, _ = self.store(fake)
        s.upload('app:/maps/tiny.zip', 'C:/dist/tiny.full.zip')
        cmd, batch = fake.calls[0]
        self.assertEqual(cmd[0], 'sftp')
        self.assertEqual(cmd[-1], 'maps@203.0.113.5')
        for opt in (['-i', '/k/key'], ['-P', '2222'], ['-o', 'UserKnownHostsFile=/k/hosts'], ['-o', 'BatchMode=yes'],
                    ['-b', '-']):
            self.assertIn(opt, [cmd[i:i + 2] for i in range(len(cmd) - 1)])
        self.assertEqual(batch, 'put "C:/dist/tiny.full.zip" "tiny.zip.part"\nrename "tiny.zip.part" "tiny.zip"\n')

    def test_upload_again(self):
        """A failed upload asked again after the waits (the maps workflow 2026-11-02: a connection reset lost a
        region's build); still failing after the last - an error."""
        fake = FakeSftp([255, 1])
        s, slept = self.store(fake)
        s.upload('app:/maps/a.zip', 'a.zip')
        self.assertEqual((len(fake.calls), slept), (3, list(sftpstore.WAITS)))
        fake = FakeSftp([255, 255, 255])
        s, _ = self.store(fake)
        with self.assertRaises(sftpstore.StoreError):
            s.upload('app:/maps/a.zip', 'a.zip')

    def test_publish(self):
        """Its files' folder on the web: nginx's /maps/ - told in the catalog as "files"."""
        s, _ = self.store(FakeSftp())
        self.assertEqual(s.publish('app:/maps'), 'http://203.0.113.5/maps/')
        self.assertEqual(s.KEY, 'files')

    def test_from_env(self):
        """The workflow's secrets: the key's text and the server's known key into files; none - no store."""
        self.assertIsNone(sftpstore.from_env({}, '.'))
        import tempfile
        d = tempfile.mkdtemp()
        s = sftpstore.from_env({'MAPS_SSH_KEY': 'KEY\n', 'MAPS_HOST': '203.0.113.5', 'MAPS_KNOWN_HOSTS': 'HOSTS'}, d)
        self.assertEqual((s.host, s.port), ('203.0.113.5', '22'))
        with open(s.key) as f:
            self.assertEqual(f.read(), 'KEY\n')
        with open(s.hosts) as f:
            self.assertEqual(f.read(), 'HOSTS\n')


if __name__ == '__main__':
    unittest.main()
