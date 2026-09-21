# sms_server.py
from http.server import BaseHTTPRequestHandler, HTTPServer
import json

class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n).decode('utf-8', 'replace')
        print('---', self.path, flush=True)
        try:
            print(json.dumps(json.loads(body), ensure_ascii=False, indent=2), flush=True)
        except Exception as e:
            print('not json:', e, '|', body, flush=True)
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(b'{"code":0}')

HTTPServer(('0.0.0.0', 1113), H).serve_forever()
