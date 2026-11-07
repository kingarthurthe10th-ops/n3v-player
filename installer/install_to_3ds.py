#!/usr/bin/env python3
"""Installs N3VPlayer.cia on a 3DS over Wi-Fi with FBI. Only needs Python 3.8+ (no extra packages).

    python install_to_3ds.py [file.cia] [--ip 3DS_ADDRESS] [--port 8642] [--no-browser]

It shares the .cia from this PC and opens a page with the steps. Either:
  - FBI -> Remote Install -> Scan QR Code, and scan the code on the page, or
  - FBI -> Remote Install -> Receive URLs over the network: this script finds the 3DS by itself and sends the file.
When the 3DS has the whole file, the page closes and the result is printed here.
"""
import argparse, concurrent.futures, http.server, ipaddress, json, os, shutil, socket, struct, subprocess, sys
import tempfile, threading, time, webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
FBI_PORT = 5000
REMEMBER = os.path.join(tempfile.gettempdir(), 'n3v_last_3ds_ip.txt')

S = {'state': 'waiting', 'ip': None, 'sent': 0, 'size': 0, 'method': None, 'msg': '', 't0': None}
LOCK = threading.Lock()
DONE = threading.Event()


def set_state(**kw):
    with LOCK: S.update(kw)


def local_ip(target='192.168.0.1'):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target, 9)); return s.getsockname()[0]           # picks the adapter; sends nothing
    except OSError:
        return '127.0.0.1'
    finally:
        s.close()


# ------------------------------------------------------------------ web server (the page + the file)
PAGE = r"""<!doctype html><html><head><meta charset="utf-8"><title>Install N3V Player</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#f4f6fa;--card:#fff;--line:#dfe3ea;--text:#1d2433;--muted:#6b7385;--dark:#181c28;--acc:#2a9fd8;--acc2:#5ac8ff;--ok:#2e9d5b;--bad:#c8453b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 "Segoe UI",system-ui,sans-serif}
header{background:var(--dark);color:#fff;padding:18px 26px;display:flex;align-items:center;gap:16px}
header svg{flex:none}header h1{margin:0;font:600 21px "Segoe UI Semibold","Segoe UI",sans-serif}header p{margin:2px 0 0;color:var(--acc2)}
main{padding:20px 26px;max-width:980px;margin:auto}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:760px){.grid{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:18px 20px}
.card h2{margin:0 0 4px;font:600 16px "Segoe UI Semibold","Segoe UI",sans-serif}.tag{color:var(--muted);font-size:13px;margin:0 0 12px}
ol{margin:0;padding-left:20px}li{margin:5px 0}b{font-weight:600}
#qr{width:200px;height:200px;margin:14px auto 6px;display:flex;align-items:center;justify-content:center;color:var(--muted);font-size:13px;text-align:center}
.url{font:12px Consolas,monospace;color:var(--acc);word-break:break-all;text-align:center}
.status{margin-top:16px}.bar{height:10px;background:#e7ebf1;border-radius:5px;overflow:hidden;margin-top:8px}
.bar i{display:block;height:100%;width:0;background:var(--acc);transition:width .3s}
#st{font-weight:600}#st.ok{color:var(--ok)}#st.bad{color:var(--bad)}#sub{color:var(--muted);font-size:13px}
.note{color:var(--muted);font-size:13px;margin-top:14px}
</style></head><body>
<header><svg width="44" height="44" viewBox="0 0 44 44"><rect x="1" y="1" width="42" height="42" rx="8" fill="none" stroke="#5ac8ff" stroke-width="2"/>
<g fill="#5ac8ff"><rect x="5" y="7" width="4" height="6"/><rect x="5" y="19" width="4" height="6"/><rect x="5" y="31" width="4" height="6"/>
<rect x="35" y="7" width="4" height="6"/><rect x="35" y="19" width="4" height="6"/><rect x="35" y="31" width="4" height="6"/></g>
<path d="M17 12v20l15-10z" fill="#fff"/></svg>
<div><h1>Install __NAME__ on your 3DS</h1><p>Use FBI on the 3DS. Your PC and 3DS must be on the same Wi-Fi.</p></div></header>
<main><div class="grid">
<div class="card"><h2>Option 1: scan a QR code</h2><p class="tag">Works with any FBI version that has Remote Install</p>
<ol><li>On the 3DS, open <b>FBI</b>.</li><li>Choose <b>Remote Install</b>, then <b>Scan QR Code</b>.</li>
<li>Point the camera at this code, then press <b>A</b> to install.</li></ol>
<div id="qr">Loading QR code...</div><div class="url">__URL__</div></div>
<div class="card"><h2>Option 2: automatic</h2><p class="tag">No camera needed</p>
<ol><li>On the 3DS, open <b>FBI</b>.</li><li>Choose <b>Remote Install</b>, then <b>Receive URLs over the network</b>.</li>
<li>Wait a few seconds: this PC finds the 3DS and sends the file.</li><li>When FBI asks, press <b>A</b> to install.</li></ol>
<p class="note">Looking for the 3DS on this network<span id="dots"></span></p></div></div>
<div class="card status"><div id="st">Waiting for the 3DS...</div><div id="sub">__SIZE__</div><div class="bar"><i id="bar"></i></div></div>
<p class="note">If nothing happens after you press A: Windows Firewall may be blocking Python. Allow it when Windows asks, then try again.
This page closes by itself when the install is done.</p>
</main>
<script src="https://cdnjs.cloudflare.com/ajax/libs/qrcodejs/1.0.0/qrcode.min.js"></script>
<script>
try{document.getElementById('qr').textContent='';new QRCode(document.getElementById('qr'),{text:'__URL__',width:200,height:200,correctLevel:QRCode.CorrectLevel.M});}
catch(e){document.getElementById('qr').textContent='QR code needs internet on this PC. Use option 2, or type the address below into FBI.';}
let n=0;setInterval(()=>{document.getElementById('dots').textContent='.'.repeat(n++%4)},500);
async function poll(){
 try{const s=await (await fetch('/status',{cache:'no-store'})).json();
  const st=document.getElementById('st'),sub=document.getElementById('sub'),mb=x=>(x/1048576).toFixed(1)+' MB';
  st.className='';
  if(s.state=='found'){st.textContent='Found the 3DS at '+s.ip+'. Press A on the 3DS to install.';}
  else if(s.state=='downloading'){st.textContent='Sending to the 3DS'+(s.ip?' at '+s.ip:'')+'...';sub.textContent=mb(s.sent)+' of '+mb(s.size);
   document.getElementById('bar').style.width=(100*s.sent/s.size)+'%';}
  else if(s.state=='done'){st.className='ok';st.textContent='Installed! '+s.msg;sub.textContent='This page will close now.';
   document.getElementById('bar').style.width='100%';setTimeout(()=>window.close(),1500);return;}
  else if(s.state=='failed'){st.className='bad';st.textContent=s.msg;return;}
 }catch(e){document.getElementById('st').textContent='Finished. You can close this page.';return;}
 setTimeout(poll,400);}
poll();
</script></body></html>"""


class OwnPortServer(http.server.ThreadingHTTPServer):
    """Refuses a port another program already has (Python's default lets Windows share it silently)."""
    allow_reuse_address = False

    def server_bind(self):
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def make_handler(path, name, url):
    size = os.path.getsize(path)
    page = (PAGE.replace('__NAME__', os.path.splitext(name)[0]).replace('__URL__', url)
            .replace('__SIZE__', '%s, %.1f MB' % (name, size / 1048576))).encode('utf-8')

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass

        def reply(self, body, ctype):
            self.send_response(200); self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body))); self.send_header('Cache-Control', 'no-store')
            self.end_headers(); self.wfile.write(body)

        def do_GET(self):
            p = self.path.split('?')[0]
            if p == '/':
                return self.reply(page, 'text/html; charset=utf-8')
            if p == '/status':
                with LOCK: return self.reply(json.dumps(S).encode(), 'application/json')
            if p.lstrip('/') != name:
                return self.send_error(404)
            start, end = 0, size - 1
            rng = self.headers.get('Range')
            if rng and rng.startswith('bytes='):
                a, _, b = rng[6:].partition('-')
                start, end = int(a or 0), int(b) if b else size - 1
                self.send_response(206); self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
            else:
                self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(end - start + 1)); self.end_headers()
            if self.command == 'HEAD': return
            who = self.client_address[0]
            with LOCK:
                if S['state'] in ('waiting', 'found'):
                    S.update(state='downloading', ip=S['ip'] or who, t0=time.time(), method=S['method'] or 'QR code')
            left = end - start + 1
            try:
                with open(path, 'rb') as f:
                    f.seek(start)
                    while left > 0:
                        chunk = f.read(min(65536, left))
                        if not chunk: break
                        self.wfile.write(chunk); left -= len(chunk)
                        set_state(sent=end - left + 1)
            except OSError:
                return                                              # the 3DS stopped (cancelled or lost Wi-Fi)
            if end == size - 1 and left == 0 and S['method'] == 'QR code':
                time.sleep(1.5)                                     # FBI writes the last part, then it's done
                finish(True, who)

        def do_HEAD(self): self.do_GET()
    return H


def finish(ok, ip, msg=None):
    with LOCK:
        if S['state'] in ('done', 'failed'): return
        secs = time.time() - (S['t0'] or time.time())
        S.update(state='done' if ok else 'failed', ip=ip,
                 msg=msg or ('The 3DS at %s received all %.1f MB in %.0f s.' % (ip, S['size'] / 1048576, secs)))
    DONE.set()


# ------------------------------------------------------------------ finding FBI's "Receive URLs over the network"
def try_fbi(ip, payload, timeout=0.5):
    """Connect to port 5000 and, if it answers, send the URL at once (so FBI doesn't log a failed request)."""
    try:
        s = socket.create_connection((ip, FBI_PORT), timeout=timeout)
    except OSError:
        return None
    try:
        s.sendall(struct.pack('!L', len(payload)) + payload); return s
    except OSError:
        s.close(); return None


def remote_install(url, fixed_ip):
    payload = url.encode('ascii')
    tried_bad = set()
    while not DONE.is_set():
        if S['state'] == 'downloading' and S['method'] == 'QR code':
            time.sleep(1); continue
        cands = [fixed_ip] if fixed_ip else []
        if not fixed_ip:
            if os.path.exists(REMEMBER):
                cands.append(open(REMEMBER).read().strip())
            me = local_ip()
            cands += [str(h) for h in ipaddress.ip_network(me + '/24', strict=False).hosts() if str(h) != me]
            cands = [c for c in dict.fromkeys(cands) if c and c not in tried_bad]
        sock, ip = None, None
        with concurrent.futures.ThreadPoolExecutor(64) as ex:
            futs = {ex.submit(try_fbi, c, payload): c for c in cands}
            for fu in concurrent.futures.as_completed(futs):
                s = fu.result()
                if s and sock is None: sock, ip = s, futs[fu]
                elif s: s.close()
        if not sock:
            time.sleep(2); continue
        set_state(state='found', ip=ip, method='Remote Install')
        print('\nFound FBI at %s. Press A on the 3DS to install.' % ip)
        sock.settimeout(1.0); t_found = time.time(); timedout = False
        while not DONE.is_set():
            try:
                sock.recv(1)                                        # FBI replies (or hangs up) when it's finished,
                if S['state'] == 'downloading' and S['sent'] >= S['size']:      # also when B cancelled it, so
                    remember(ip); finish(True, ip); break                       # only a full download counts
                sock.close(); sock = None; break                    # cancelled on the 3DS: keep waiting
            except socket.timeout:
                if S['state'] == 'found' and time.time() - t_found > 120:
                    timedout = True; break                                           # nobody pressed A (or it wasn't FBI)
            except OSError:
                break
        if sock: sock.close()
        if not DONE.is_set():
            if timedout and not fixed_ip:
                tried_bad.add(ip)                                   # never started downloading: skip it from now on
            set_state(state='waiting', sent=0, method=None, ip=None)
            print('\nThe device at %s didn\'t install it (cancelled, or not FBI). Still waiting...' % ip)
            time.sleep(3)


def remember(ip):
    if not ip.startswith('127.'):                                   # never this PC itself (a test stand-in)
        try: open(REMEMBER, 'w').write(ip)
        except OSError: pass


# ------------------------------------------------------------------ the page window
def open_page(url):
    """Opens the page in its own Edge/Chrome app window (so it can be closed again), else the normal browser."""
    cands = []
    if os.name == 'nt':
        for base in (os.environ.get('ProgramFiles(x86)', ''), os.environ.get('ProgramFiles', ''), os.environ.get('LOCALAPPDATA', '')):
            cands += [os.path.join(base, r'Microsoft\Edge\Application\msedge.exe'), os.path.join(base, r'Google\Chrome\Application\chrome.exe')]
    else:
        cands += [shutil.which(x) or '' for x in ('google-chrome', 'chromium', 'chromium-browser', 'microsoft-edge')]
    for exe in cands:
        if exe and os.path.exists(exe):
            prof = tempfile.mkdtemp(prefix='n3v_page_')
            try:
                p = subprocess.Popen([exe, '--app=' + url, '--user-data-dir=' + prof, '--no-first-run', '--disable-sync',
                                      '--inprivate' if 'msedge' in exe else '--incognito',
                                      '--no-default-browser-check', '--window-size=1000,780'],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return p, prof
            except OSError:
                continue
    webbrowser.open(url)
    return None, None


def close_page(proc, prof):
    """Edge can hand its window to a second copy of itself, so stopping proc alone may leave the page open:
    also stop every browser process using this page's own private profile folder."""
    if not proc: return
    proc.terminate()
    try: proc.wait(5)
    except subprocess.TimeoutExpired: proc.kill()
    if os.name == 'nt':
        tag = os.path.basename(prof)
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='msedge.exe' or Name='chrome.exe'\" | "
              "Where-Object { $_.CommandLine -like '*%s*' } | "
              "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" % tag)
        try: subprocess.run(['powershell', '-NoProfile', '-Command', ps], timeout=20, creationflags=0x08000000,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except (OSError, subprocess.TimeoutExpired): pass
    time.sleep(0.5)
    shutil.rmtree(prof, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description='Install a .cia on your 3DS with FBI over Wi-Fi.')
    ap.add_argument('cia', nargs='?', help='the .cia to install (default: N3VPlayer.cia next to this script)')
    ap.add_argument('--ip', help="the 3DS's address, if it can't be found by itself (FBI shows it)")
    ap.add_argument('--port', type=int, default=8642)
    ap.add_argument('--no-browser', action='store_true', help="don't open the page")
    ap.add_argument('--stop-file', help='stop (and close the page) when this file appears; used by the converter')
    a = ap.parse_args()
    path = a.cia
    if not path:
        for c in (os.path.join(HERE, 'N3VPlayer.cia'), os.path.join(HERE, '..', 'dist', 'N3VPlayer.cia')):
            if os.path.exists(c): path = c; break
    if not path or not os.path.exists(path):
        sys.exit('Could not find the .cia file. Put install_to_3ds.py next to N3VPlayer.cia, or give the path.')
    path = os.path.abspath(path); name = os.path.basename(path); size = os.path.getsize(path)
    set_state(size=size)

    lan = local_ip(a.ip or '192.168.0.1')
    server = None
    for port in [a.port] + list(range(a.port + 1, a.port + 20)):
        try:
            server = OwnPortServer(('0.0.0.0', port), make_handler(path, name, 'http://%s:%d/%s' % (lan, port, name)))
            break
        except OSError:
            continue
    if not server: sys.exit('Could not open a port for the web server.')
    port = server.server_address[1]
    url = 'http://%s:%d/%s' % (lan, port, name)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    print('Installing %s (%.1f MB) on your 3DS with FBI.' % (name, size / 1048576))
    print('  Option 1: FBI -> Remote Install -> Scan QR Code (the code is on the page that just opened)')
    print('  Option 2: FBI -> Remote Install -> Receive URLs over the network (found automatically)')
    print('  The file is shared at %s   (Ctrl+C to stop)' % url)
    proc, prof = (None, None) if a.no_browser else open_page('http://127.0.0.1:%d/' % port)
    threading.Thread(target=remote_install, args=(url, a.ip), daemon=True).start()

    last = -1
    try:
        while not DONE.wait(0.5):
            if a.stop_file and os.path.exists(a.stop_file): raise KeyboardInterrupt
            if S['state'] == 'downloading':
                pct = S['sent'] * 100 // max(1, size)
                if pct != last:
                    last = pct
                    print('\r  sending to the 3DS (%s): %3d%%  (%.1f of %.1f MB)' % (S['method'], pct, S['sent'] / 1048576, size / 1048576), end='', flush=True)
    except KeyboardInterrupt:
        print('\nStopped. Nothing was installed.' if S['state'] != 'done' else '')
        close_page(proc, prof)
        return 1
    print()
    if S['state'] == 'done':
        print('Done: %s is installed. %s' % (name, S['msg']))
        print('It\'s on the 3DS HOME Menu now (FBI may show "Install succeeded" first; press B to leave FBI).')
    else:
        print('Failed: %s' % S['msg'])
    time.sleep(2.5)                                                 # let the page show the result, then close it
    close_page(proc, prof)
    server.shutdown()
    return 0 if S['state'] == 'done' else 1


if __name__ == '__main__':
    sys.exit(main())
