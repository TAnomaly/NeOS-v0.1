#!/usr/bin/env python3
"""
NeOS web bridge: TCP listener for NeOS + HTTP UI to view live data.

- TCP server: 0.0.0.0:8080  (NeOS connects here, sends bytes)
- HTTP server: 0.0.0.0:8000 (browser opens this, sees live log)
- POST /send sends a line back to the connected NeOS socket.

Usage:
    python3 tools/neos_web.py
    # then on NeOS: `tcptest` (connects to 192.168.50.1:8080)
    # open browser: http://192.168.50.1:8000
"""

import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

TCP_HOST = '0.0.0.0'
TCP_PORT = 8080
HTTP_HOST = '0.0.0.0'
HTTP_PORT = 8000

state = {
    'log': [],          # list of (timestamp, direction, text)
    'client': None,     # current connected NeOS socket
    'addr':   None,
}
lock = threading.Lock()


def log_line(direction, text):
    with lock:
        state['log'].append((time.strftime('%H:%M:%S'), direction, text))
        if len(state['log']) > 500:
            state['log'] = state['log'][-500:]


def tcp_listener():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((TCP_HOST, TCP_PORT))
    s.listen(1)
    print(f'[tcp] listening on {TCP_HOST}:{TCP_PORT}')
    while True:
        client, addr = s.accept()
        print(f'[tcp] NeOS connected from {addr}')
        with lock:
            state['client'] = client
            state['addr']   = addr
        log_line('SYS', f'connected from {addr[0]}:{addr[1]}')
        # Greet
        try:
            client.sendall(b'Welcome to NeOS web bridge!\r\n')
            log_line('OUT', 'Welcome to NeOS web bridge!')
        except OSError:
            pass
        # Read loop
        try:
            while True:
                data = client.recv(1024)
                if not data:
                    break
                text = data.decode('utf-8', errors='replace')
                log_line('IN', text)
                print(f'[tcp] RX {len(data)}B: {text!r}')
        except OSError as e:
            print(f'[tcp] error: {e}')
        finally:
            log_line('SYS', 'disconnected')
            with lock:
                state['client'] = None
                state['addr']   = None
            try:
                client.close()
            except OSError:
                pass


HTML = """<!DOCTYPE html>
<html><head>
<meta charset="utf-8"><title>NeOS web bridge</title>
<style>
  body { font: 13px/1.4 monospace; background: #1a0014; color: #eee; margin: 0; padding: 16px; }
  h1   { color: #e95420; margin-top: 0; font-size: 18px; }
  #log { background: #2c001e; border: 1px solid #4a0e2e; padding: 10px;
         height: 60vh; overflow: auto; white-space: pre-wrap; }
  .ts  { color: #888; }
  .IN  { color: #6bb7ff; }
  .OUT { color: #50c878; }
  .SYS { color: #e1bc4e; }
  form { margin-top: 12px; display: flex; gap: 8px; }
  input{ flex: 1; background: #2c001e; color: #fff; border: 1px solid #4a0e2e;
         padding: 8px; font: 13px monospace; }
  button { background: #e95420; color: #fff; border: none; padding: 8px 16px;
           cursor: pointer; font: 13px monospace; }
  button:hover { background: #dd4814; }
  .status { color: #aaa; margin-bottom: 8px; }
</style>
</head><body>
<h1>NeOS &mdash; web bridge</h1>
<div class="status" id="status">disconnected</div>
<div id="log">(no log yet)</div>
<form id="sendform">
  <input id="msg" type="text" placeholder="type to send to NeOS" autocomplete="off">
  <button type="submit">Send</button>
</form>
<script>
async function poll() {
  try {
    const r = await fetch('/log');
    const j = await r.json();
    const log = document.getElementById('log');
    log.innerHTML = j.lines.map(l =>
      `<span class="ts">${l[0]}</span> <span class="${l[1]}">[${l[1]}]</span> ${escapeHtml(l[2])}`
    ).join('\\n');
    log.scrollTop = log.scrollHeight;
    document.getElementById('status').textContent = j.client ? `connected: ${j.addr}` : 'disconnected';
  } catch(e) {}
}
function escapeHtml(s) {
  return s.replace(/[&<>"']/g, c =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
document.getElementById('sendform').addEventListener('submit', async (e) => {
  e.preventDefault();
  const msg = document.getElementById('msg').value;
  if (!msg) return;
  await fetch('/send', { method: 'POST',
    headers: {'Content-Type':'application/x-www-form-urlencoded'},
    body: 'msg=' + encodeURIComponent(msg) });
  document.getElementById('msg').value = '';
  poll();
});
setInterval(poll, 500);
poll();
</script>
</body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path == '/' or self.path == '/index.html':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.end_headers()
            self.wfile.write(HTML.encode('utf-8'))
            return
        if self.path == '/log':
            with lock:
                lines = state['log'][:]
                client = state['client'] is not None
                addr = f"{state['addr'][0]}:{state['addr'][1]}" if state['addr'] else ''
            import json
            body = json.dumps({'lines': lines, 'client': client, 'addr': addr})
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(body.encode('utf-8'))
            return
        self.send_error(404)

    def do_POST(self):
        if self.path == '/send':
            length = int(self.headers.get('Content-Length', 0))
            raw = self.rfile.read(length).decode('utf-8')
            params = parse_qs(raw)
            msg = params.get('msg', [''])[0]
            if msg:
                with lock:
                    client = state['client']
                if client:
                    try:
                        client.sendall((msg + '\r\n').encode('utf-8'))
                        log_line('OUT', msg)
                    except OSError as e:
                        log_line('SYS', f'send error: {e}')
                else:
                    log_line('SYS', 'send dropped (no NeOS connected)')
            self.send_response(204)
            self.end_headers()
            return
        self.send_error(404)


def main():
    t = threading.Thread(target=tcp_listener, daemon=True)
    t.start()
    server = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), Handler)
    print(f'[http] open http://{HTTP_HOST}:{HTTP_PORT} (or http://192.168.50.1:{HTTP_PORT})')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nbye')


if __name__ == '__main__':
    main()
