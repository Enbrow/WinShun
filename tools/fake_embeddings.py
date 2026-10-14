"""A fake OpenAI-compatible /v1/embeddings server for testing winshun.semantic.

Vectors are hashed character bigrams (lexical similarity, not meaning), 256
dimensions. Counts requests and inputs; GET /stats prints them. Usage:
    python tools/fake_embeddings.py 18080
    wsbench --semantic --setting provider=custom --setting endpoint=http://127.0.0.1:18080/v1 --setting model=fake "QUERY"
"""
import hashlib, json, math, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DIMS = 256
stats = {'requests': 0, 'inputs': 0, 'chars': 0, 'max_batch': 0}
lock = threading.Lock()


def vector(text):
    v = [0.0] * DIMS
    t = text.lower()
    for i in range(len(t) - 1):
        h = int.from_bytes(hashlib.md5(t[i:i + 2].encode('utf-8')).digest()[:4], 'little')
        v[h % DIMS] += 1.0 if (h >> 31) & 1 else -1.0
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        body = json.dumps(stats).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        req = json.loads(self.rfile.read(n))
        if self.path.rstrip('/') != '/v1/embeddings' or req.get('model') != 'fake':
            self.send_response(404)
            body = json.dumps({'error': {'message': 'no such model or path: %s %s' % (self.path, req.get('model'))}}).encode()
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        inputs = req['input'] if isinstance(req['input'], list) else [req['input']]
        with lock:
            stats['requests'] += 1
            stats['inputs'] += len(inputs)
            stats['chars'] += sum(len(x) for x in inputs)
            stats['max_batch'] = max(stats['max_batch'], len(inputs))
        data = [{'object': 'embedding', 'index': i, 'embedding': vector(x)} for i, x in enumerate(inputs)]
        body = json.dumps({'object': 'list', 'data': data, 'model': 'fake'}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
print('fake embeddings on http://127.0.0.1:%d/v1' % port, flush=True)
ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
