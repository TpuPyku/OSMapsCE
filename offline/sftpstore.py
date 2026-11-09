"""The maps' own server (the user 2026-11-09: Yandex Disk's upload limit - per 30 days - ran out): the regions whole
put there by SFTP, served by its nginx over plain HTTP (the unit has no HTTPS of its own; the server has no name).

    Store(host, port, key, hosts).upload('app:/maps/a.zip', 'a.zip')   as <its files>/a.zip
    Store(...).publish('app:/maps')                                    -> 'http://<host>/maps/' (the catalog's "files")

The server's user "maps": SFTP only, locked in its folder (the uploads land in it), by the key alone. The workflow's
secrets: MAPS_SSH_KEY (the key), MAPS_HOST, MAPS_PORT (22 if none), MAPS_KNOWN_HOSTS (ssh-keyscan's lines).
"""
import os
import subprocess
import time

USER = 'maps'
WAITS = (30, 60)   # the upload asked again after these seconds (as yadisk's: a connection reset)


class StoreError(Exception):
    pass


def sftp_run(cmd, batch):
    """The sftp's exit code, its batch on stdin."""
    return subprocess.run(cmd, input=batch.encode()).returncode   # (the job's own time limit: 6 h)


class Store:
    KEY = 'files'   # (the catalog's: Yandex Disk's is "disk")

    def __init__(self, host, port, key, hosts, run=sftp_run, sleep=time.sleep):
        self.host, self.port, self.key, self.hosts, self.run, self.sleep = host, port, key, hosts, run, sleep

    def command(self):
        return ['sftp', '-b', '-', '-i', self.key, '-P', self.port, '-o', 'BatchMode=yes',
                '-o', 'UserKnownHostsFile=' + self.hosts, '-o', 'StrictHostKeyChecking=yes', USER + '@' + self.host]

    def upload(self, path, file):
        """The file in as path's name - name.part first, renamed over the old one (a download meanwhile: the old one
        whole); a failure asked again after WAITS."""
        name = path.split('/')[-1]
        batch = 'put "%s" "%s.part"\nrename "%s.part" "%s"\n' % (file, name, name, name)
        for wait in WAITS + (None,):
            code = self.run(self.command(), batch)
            if code == 0:
                return
            if wait is None:
                raise StoreError('sftp %s: exit %d' % (name, code))
            self.sleep(wait)

    def publish(self, path):
        """The files' folder on the web (nginx's /maps/; path: the disk's, not used)."""
        return 'http://%s/maps/' % self.host


def from_env(env, folder):
    """The store of the workflow's secrets (the key and the known hosts written into folder); None without them."""
    if not env.get('MAPS_SSH_KEY') or not env.get('MAPS_HOST'):
        return None
    files = []
    for name, text in (('maps_key', env['MAPS_SSH_KEY']), ('maps_hosts', env.get('MAPS_KNOWN_HOSTS', ''))):
        p = os.path.join(folder, name)
        with open(p, 'w', newline='\n') as f:
            f.write(text.rstrip('\n') + '\n')
        os.chmod(p, 0o600)   # (ssh refuses a key others can read)
        files.append(p)
    return Store(env['MAPS_HOST'], env.get('MAPS_PORT') or '22', files[0], files[1])
