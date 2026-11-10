"""A fake Google (sign-in + Drive) and Amazon S3 server for testing Canivete's uploads offline.

    python tools/upload_mock.py <port> <S3 secret key>

Canivete test copies talk to it when CANIVETE_UPLOAD_MOCK=http://127.0.0.1:<port> is set. It checks
what the real services would (PKCE, state, tokens, the resumable protocol, AWS signatures) and keeps
what it received. Test control:
    GET  /_ctl/state            what it saw (files, sizes, SHA-256, errors) as JSON
    POST /_ctl/mode  {json}     faults: {"break_piece": n} cuts the n-th upload piece halfway once,
                                {"refresh": "invalid_grant"}, {"s3": "AccessDenied"}, {"share": 403}
"""
import base64, hashlib, hmac, json, secrets, sys, threading, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
S3_SECRET = sys.argv[2]
LOCK = threading.Lock()
STATE = {'errors': [], 'files': [], 'folders': {}, 'shared': [], 'revoked': 0, 'about': 0, 'token_calls': 0, 'pieces': 0}
MODE = {}
CODES = {}       # code -> (challenge, redirect_uri, client_id)
REFRESH = set()
ACCESS = set()
SESSIONS = {}    # sid -> dict(name, parents, mime, size, data)


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b'=').decode()


def err(msg):
    with LOCK:
        STATE['errors'].append(msg)
    print('MOCK ERROR:', msg, flush=True)


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        pass

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n else b''

    def send(self, code, data=b'', ctype='application/json', headers=None):
        if isinstance(data, (dict, list)):
            data = json.dumps(data).encode()
        elif isinstance(data, str):
            data = data.encode()
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if data:
            self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        if data:
            self.wfile.write(data)

    def bearer_ok(self):
        a = self.headers.get('Authorization', '')
        if not a.startswith('Bearer ') or a[7:] not in ACCESS:
            err('bad bearer token on ' + self.path)
            self.send(401, {'error': {'code': 401, 'message': 'Invalid Credentials'}})
            return False
        return True

    # ---- GET ----
    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        if u.path == '/_ctl/state':
            with LOCK:
                return self.send(200, STATE)
        if u.path == '/o/oauth2/v2/auth':  # the "browser" step: approve and send back to the app
            for k, v in (('response_type', 'code'), ('code_challenge_method', 'S256'), ('access_type', 'offline'),
                         ('scope', 'https://www.googleapis.com/auth/drive.file')):
                if q.get(k) != v:
                    err(f'auth: {k}={q.get(k)!r}')
            redirect = q.get('redirect_uri', '')
            if not redirect.startswith('http://127.0.0.1:'):
                err('auth: redirect ' + redirect)
            if len(q.get('code_challenge', '')) != 43 or not q.get('state'):
                err('auth: challenge/state missing')
            code = secrets.token_urlsafe(16)
            CODES[code] = (q.get('code_challenge'), redirect, q.get('client_id'))
            return self.send(302, headers={'Location': redirect + '/?' + urllib.parse.urlencode(
                {'state': q.get('state', ''), 'code': code, 'scope': q.get('scope', '')})})
        if u.path == '/drive/v3/about':
            if not self.bearer_ok():
                return
            STATE['about'] += 1
            return self.send(200, {'user': {'emailAddress': 'tester@example.com'}})
        if u.path == '/drive/v3/files':
            if not self.bearer_ok():
                return
            m = q.get('q', '')
            if "mimeType='application/vnd.google-apps.folder'" not in m or 'trashed=false' not in m:
                err('folder search: ' + m)
            found = [{'id': i} for i, n in STATE['folders'].items() if f"name='{n}'" in m]
            return self.send(200, {'files': found})
        err('unknown GET ' + self.path)
        self.send(404, {'error': {'message': 'not found'}})

    # ---- POST ----
    def do_POST(self):
        u = urllib.parse.urlsplit(self.path)
        data = self.body()
        if u.path == '/_ctl/mode':
            MODE.update(json.loads(data or b'{}'))
            return self.send(200, MODE)
        if u.path == '/token':
            STATE['token_calls'] += 1
            f = dict(urllib.parse.parse_qsl(data.decode()))
            if not f.get('client_id') or not f.get('client_secret'):
                err('token: no client id/secret')
            if f.get('grant_type') == 'authorization_code':
                c = CODES.pop(f.get('code', ''), None)
                if not c:
                    return self.send(400, {'error': 'invalid_grant', 'error_description': 'Bad code'})
                challenge, redirect, client = c
                if b64url(hashlib.sha256(f.get('code_verifier', '').encode()).digest()) != challenge:
                    err('token: PKCE verifier does not match')
                    return self.send(400, {'error': 'invalid_grant', 'error_description': 'PKCE'})
                if f.get('redirect_uri') != redirect or f.get('client_id') != client:
                    err('token: redirect/client mismatch')
                    return self.send(400, {'error': 'redirect_uri_mismatch'})
                r, a = secrets.token_urlsafe(24), secrets.token_urlsafe(24)
                REFRESH.add(r)
                ACCESS.add(a)
                return self.send(200, {'access_token': a, 'refresh_token': r, 'expires_in': 3599,
                                       'token_type': 'Bearer', 'scope': 'https://www.googleapis.com/auth/drive.file'})
            if f.get('grant_type') == 'refresh_token':
                if MODE.get('refresh') == 'invalid_grant' or f.get('refresh_token') not in REFRESH:
                    return self.send(400, {'error': 'invalid_grant',
                                           'error_description': 'Token has been expired or revoked.'})
                a = secrets.token_urlsafe(24)
                ACCESS.add(a)
                return self.send(200, {'access_token': a, 'expires_in': 3599, 'token_type': 'Bearer'})
            err('token: grant ' + str(f.get('grant_type')))
            return self.send(400, {'error': 'unsupported_grant_type'})
        if u.path == '/revoke':
            tok = dict(urllib.parse.parse_qsl(data.decode())).get('token', '')
            if tok in REFRESH:
                REFRESH.discard(tok)
                STATE['revoked'] += 1
            return self.send(200, {})
        if u.path == '/drive/v3/files':  # make a folder
            if not self.bearer_ok():
                return
            j = json.loads(data)
            if j.get('mimeType') != 'application/vnd.google-apps.folder':
                err('folder create: ' + data.decode())
            fid = 'folder' + str(len(STATE['folders']) + 1)
            STATE['folders'][fid] = j.get('name')
            return self.send(200, {'id': fid})
        if u.path == '/upload/drive/v3/files':  # start a resumable upload
            if not self.bearer_ok():
                return
            q = dict(urllib.parse.parse_qsl(u.query))
            if q.get('uploadType') != 'resumable':
                err('upload: not resumable')
            j = json.loads(data)
            parents = j.get('parents', [])
            if any(p not in STATE['folders'] for p in parents):
                return self.send(404, {'error': {'code': 404, 'message': 'File not found: parent'}})
            sid = secrets.token_hex(8)
            SESSIONS[sid] = {'name': j.get('name'), 'parents': parents, 'size': int(self.headers['X-Upload-Content-Length']),
                             'mime': self.headers.get('X-Upload-Content-Type'), 'data': bytearray()}
            return self.send(200, headers={'Location': f'http://127.0.0.1:{PORT}/upload-session/{sid}'})
        if u.path.startswith('/drive/v3/files/') and u.path.endswith('/permissions'):
            if not self.bearer_ok():
                return
            if MODE.get('share'):
                return self.send(int(MODE['share']), {'error': {'code': 403, 'message': 'Sharing is off for this domain'}})
            j = json.loads(data)
            if j != {'role': 'reader', 'type': 'anyone'}:
                err('share: ' + data.decode())
            STATE['shared'].append(u.path.split('/')[4])
            return self.send(200, {'id': 'anyoneWithLink'})
        err('unknown POST ' + self.path)
        self.send(404, {})

    # ---- PUT ----
    def do_PUT(self):
        u = urllib.parse.urlsplit(self.path)
        if u.path.startswith('/upload-session/'):
            return self.piece(u.path.split('/')[-1])
        return self.s3_put(u)

    def piece(self, sid):
        s = SESSIONS.get(sid)
        data = self.body()
        if not s:
            return self.send(404, {'error': {'message': 'no session'}})
        cr = self.headers.get('Content-Range', '')
        have = len(s['data'])
        def partial():
            h = {'Range': f'bytes=0-{len(s["data"]) - 1}'} if s['data'] else {}
            return self.send(308, headers=h)
        if cr.startswith('bytes */'):
            return partial()
        rng, total = cr[6:].split('/')
        a, b = (int(x) for x in rng.split('-'))
        if int(total) != s['size'] or b - a + 1 != len(data) or a != have:
            err(f'piece: range {cr} with {len(data)} bytes, have {have}')
            return self.send(400, {'error': {'message': 'bad range'}})
        STATE['pieces'] += 1
        if MODE.get('break_piece') == STATE['pieces']:
            MODE.pop('break_piece')
            s['data'] += data[:len(data) // 2]  # half of it got there, then the line "broke"
            return self.send(503, {'error': {'message': 'Backend Error'}})
        s['data'] += data
        if len(s['data']) < s['size']:
            return partial()
        fid = 'file' + sid
        with LOCK:
            STATE['files'].append({'service': 'drive', 'name': s['name'], 'parents': s['parents'], 'mime': s['mime'],
                                   'size': len(s['data']), 'sha256': hashlib.sha256(s['data']).hexdigest(), 'id': fid})
        return self.send(200, {'id': fid, 'webViewLink': f'https://drive.google.com/file/d/{fid}/view?usp=drivesdk'})

    def s3_put(self, u):
        data = self.body()
        a = self.headers.get('Authorization', '')
        try:
            cred = a.split('Credential=')[1].split(',')[0]
            signed = a.split('SignedHeaders=')[1].split(',')[0]
            sig = a.split('Signature=')[1].strip()
            key_id, date, region, service, term = cred.split('/')
        except Exception:
            err('s3: bad Authorization ' + a)
            return self.send(400, '<Error><Code>InvalidRequest</Code></Error>', 'application/xml')
        amz = self.headers.get('x-amz-date', '')
        canon = 'PUT\n' + u.path + '\n' + u.query + '\n'
        for h in signed.split(';'):
            canon += h + ':' + ' '.join((self.headers.get(h) or '').split()) + '\n'
        canon += '\n' + signed + '\n' + self.headers.get('x-amz-content-sha256', '')
        scope = f'{date}/{region}/{service}/{term}'
        to_sign = 'AWS4-HMAC-SHA256\n' + amz + '\n' + scope + '\n' + hashlib.sha256(canon.encode()).hexdigest()
        k = ('AWS4' + S3_SECRET).encode()
        for part in (date, region, service, term):
            k = hmac.new(k, part.encode(), hashlib.sha256).digest()
        good = hmac.new(k, to_sign.encode(), hashlib.sha256).hexdigest()
        if 'host' not in signed.split(';') or service != 's3' or not amz.startswith(date):
            err('s3: signed headers / scope ' + a)
        if MODE.get('s3') == 'AccessDenied':
            return self.send(403, '<?xml version="1.0"?><Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>',
                             'application/xml')
        if sig != good:
            return self.send(403, '<Error><Code>SignatureDoesNotMatch</Code><Message>The request signature we calculated '
                                  'does not match</Message></Error>', 'application/xml')
        bucket, key = u.path.lstrip('/').split('/', 1)
        with LOCK:
            STATE['files'].append({'service': 's3', 'bucket': bucket, 'key': urllib.parse.unquote(key), 'region': region,
                                   'mime': self.headers.get('Content-Type'), 'size': len(data),
                                   'sha256': hashlib.sha256(data).hexdigest()})
        return self.send(200, headers={'ETag': '"' + hashlib.md5(data).hexdigest() + '"'})


ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
