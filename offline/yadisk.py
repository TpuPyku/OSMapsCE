"""Yandex Disk's REST API, the little the maps need (the user 2026-10-30: the regions whole - the graph, the search,
the tiles - on the user's Yandex Disk: GitHub's downloads are slow): a file up, a folder public, a public file's
download link.

    Disk(token).upload('app:/maps/a.zip', 'a.zip')   the folders on the way made
    Disk(token).publish('app:/maps')                 -> its public link
    public_href(public_url, 'a.zip')                 -> the file's download link (no token)

The token: an OAuth token of the disk's owner with cloud_api:disk.app_folder - its app's folder only, app:/ (the
user 2026-10-30: no access to the rest of the disk); the workflow's secret YANDEX_DISK_TOKEN.
"""
import json
import urllib.error
import urllib.parse
import urllib.request

API = 'https://cloud-api.yandex.net/v1/disk/'


class DiskError(Exception):
    pass


def urllib_send(method, url, headers, body=None):
    """(status, bytes) of a request; an HTTP error's status and body too."""
    req = urllib.request.Request(url, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def answer(status, data, what, ok=(200, 201, 202)):
    """The JSON answer of a call that went well; else DiskError with the server's words."""
    if status not in ok:
        raise DiskError('%s: HTTP %d %s' % (what, status, data[:300].decode('utf-8', 'replace')))
    return json.loads(data) if data else {}


def call(send, method, what, params, headers, ok=(200, 201, 202)):
    status, data = send(method, API + what + '?' + urllib.parse.urlencode(params), headers)
    return answer(status, data, what, ok)


class Disk:
    def __init__(self, token, send=urllib_send):
        self.send, self.auth = send, {'Authorization': 'OAuth ' + token, 'Accept': 'application/json'}

    def folder(self, path):   # made (there already: 409 - fine)
        call(self.send, 'PUT', 'resources', {'path': path}, self.auth, ok=(201, 409))

    def upload(self, path, file):
        """The file into path (overwritten), the folders on the way made."""
        parts = path.split('/')
        for i in range(2, len(parts)):   # ('app:', 'maps', 'a.zip': app:/maps)
            self.folder('/'.join(parts[:i]))
        href = call(self.send, 'GET', 'resources/upload', {'path': path, 'overwrite': 'true'}, self.auth)['href']
        with open(file, 'rb') as f:
            status, data = self.send('PUT', href, {'Content-Type': 'application/octet-stream'}, f)
        answer(status, data, 'upload ' + path, ok=(200, 201, 202))

    def publish(self, path):
        """The folder (or file) public; its public link."""
        call(self.send, 'PUT', 'resources/publish', {'path': path}, self.auth)
        return call(self.send, 'GET', 'resources', {'path': path, 'fields': 'public_url'}, self.auth)['public_url']


def public_href(public_url, name, send=urllib_send):
    """A public folder's file's download link (lives some hours)."""
    return call(send, 'GET', 'public/resources/download', {'public_key': public_url, 'path': '/' + name},
                {'Accept': 'application/json'})['href']
