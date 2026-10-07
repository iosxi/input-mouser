"""ファイルのコピー＆貼り付けの速さと中身の一致を、遅い回線をまねて測る(1 台の PC で)。

    python tools/fxbench.py [--old build/v12] [--files 1000 | --big 10] [--rtt 1] [--mbps 0] [--repeat 3]

- 受け手を -bind 127.0.0.1 -nohook、操作する側を -nohook で動かす。フックを掛けないので、計測中に
  利用者のマウス・キーを横取りしない。切り替えもしない(受け手が入力を再現する場面は来ない)。
  受け手に -dryrun を付けないのは、-dryrun ではファイルの一覧をクリップボードに置かずログに書くだけだから。
- 操作する側の [peer] は中継(遅れ = --rtt の半分ずつ、帯域 = --mbps。0 = 絞らない)を指す。
  入力の接続もファイルの接続も中継を通る。
- テスト用のフォルダ(build/fxdata/many に細かいファイル --files 個、または --big で 2MB のファイル)を
  クリップボードに置き、操作する側に -clipsend 1 で一覧を送らせる。受け手が置いた「中身はあとから
  届くファイル」を tools/fxpaste.c(エクスプローラーの貼り付けと同じく、貼り付け先の IDropTarget へ
  落とす)で貼り付け、届くまでの時間と、元と 1 バイトずつ一致するかを見る。
- --old に古い版の input-mouser.exe の置き場所を渡すと、それとも比べる
  (git show v12:input-mouser.exe > build/v12/input-mouser.exe)。版の違う相手とはつながらないので、
  古い版どうし・この版どうしで比べる。
- 元のクリップボードの文字は最後に戻す。fxpaste.exe が無ければ gcc で作る。
"""
import argparse, asyncio, ctypes, os, random, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'input-mouser.exe')
WORK = os.path.join(ROOT, 'build', 'fxbench')
FXPASTE = os.path.join(ROOT, 'build', 'fxpaste.exe')
KEY = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff'
PORT_S, PORT_PROXY, PORT_M = 31871, 31872, 31873
QUEUE_LIMIT = 256 * 1024

WORDS = ('mouse keyboard clipboard switch peer screen edge cursor file copy paste network latency '
         'packet stream buffer thread queue chunk folder explorer windows').split()


def make_files(n, big=False):
    d = os.path.join(ROOT, 'build', 'fxdata', 'big' if big else 'many')
    os.makedirs(d, exist_ok=True)
    have = sorted(os.listdir(d))
    if len(have) != n:
        for f in have:
            os.remove(os.path.join(d, f))
        rnd = random.Random(1)
        for i in range(n):
            if big:                     # 乱数(圧縮が効かない)とテキスト(よく縮む)を交互に。2MB ずつ
                with open(os.path.join(d, f'b{i:03d}.bin'), 'wb') as f:
                    if i % 2 == 0:
                        f.write(rnd.randbytes(2 << 20))
                    else:
                        words = [rnd.choice(WORDS) if rnd.random() < 0.8 else str(rnd.randint(0, 99999)) for _ in range(400000)]
                        f.write(' '.join(words).encode()[:2 << 20].ljust(2 << 20, b'.'))
                continue
            size = rnd.randint(1024, 8192)
            text = []
            while sum(len(w) + 1 for w in text) < size:
                text.append(rnd.choice(WORDS) if rnd.random() < 0.8 else str(rnd.randint(0, 99999)))
            with open(os.path.join(d, f'f{i:05d}.txt'), 'w', newline='\n') as f:
                f.write(' '.join(text)[:size])
    total = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d))
    return d, total


def ps(cmd):
    return subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True,
                          encoding='utf-8', errors='replace').stdout


# ------------------------------------------------------------------
#  遅い回線をまねる中継(片方向ごとに、帯域ぶんの時間で送り出し、遅れを足して書く)
# ------------------------------------------------------------------

class Link:
    def __init__(self, mbps, delay):
        self.bps = mbps * 1e6 if mbps else 0
        self.delay = delay
        self.free = 0.0
        self.queued = 0

    async def run(self, reader, writer):
        q = asyncio.Queue()
        cond = asyncio.Condition()

        async def pump_in():
            while True:
                d = await reader.read(16384)
                if not d:
                    await q.put(None)
                    return
                async with cond:
                    await cond.wait_for(lambda: self.queued < QUEUE_LIMIT)
                    self.queued += len(d)
                now = time.perf_counter()
                self.free = max(now, self.free) + (len(d) * 8 / self.bps if self.bps else 0)
                await q.put((self.free + self.delay, d))

        async def pump_out():
            while True:
                it = await q.get()
                if it is None:
                    writer.close()
                    return
                at, d = it
                w = at - time.perf_counter()
                if w > 0:
                    await asyncio.sleep(w)
                writer.write(d)
                await writer.drain()
                async with cond:
                    self.queued -= len(d)
                    cond.notify_all()

        await asyncio.gather(pump_in(), pump_out(), return_exceptions=True)


def start_proxy(mbps, rtt_ms):
    ready = threading.Event()

    async def handle(cr, cw):
        try:
            sr, sw = await asyncio.open_connection('127.0.0.1', PORT_S)
        except OSError:
            cw.close()
            return
        for w in (cw, sw):
            w.get_extra_info('socket').setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        await asyncio.gather(Link(mbps, rtt_ms / 2000).run(cr, sw), Link(mbps, rtt_ms / 2000).run(sr, cw),
                             return_exceptions=True)

    async def serve():
        srv = await asyncio.start_server(handle, '127.0.0.1', PORT_PROXY)
        ready.set()
        async with srv:
            await srv.serve_forever()

    loop = asyncio.new_event_loop()
    loop.set_exception_handler(lambda l, c: None)       # 切れたときの後始末の例外は黙らせる
    threading.Thread(target=lambda: loop.run_until_complete(serve()), daemon=True).start()
    ready.wait(5)


# ------------------------------------------------------------------
#  計測
# ------------------------------------------------------------------

def clip_has_virtual_files():
    u = ctypes.windll.user32
    return bool(u.IsClipboardFormatAvailable(u.RegisterClipboardFormatW('FileGroupDescriptorW')))


def same_tree(src, dst):
    for root, _, files in os.walk(src):
        for f in files:
            a = os.path.join(root, f)
            b = os.path.join(dst, os.path.relpath(a, src))
            if not os.path.exists(b) or open(a, 'rb').read() != open(b, 'rb').read():
                return False, os.path.relpath(a, src)
    return True, None


def wait_log(path, text, sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        try:
            if text in open(path, encoding='utf-8', errors='replace').read():
                return True
        except OSError:
            pass
        time.sleep(0.05)
    return False


def run(label, exe, src, total, reverse=False):
    """reverse = 受け手(スレーブ)でコピーして、操作する側(マスター)で貼り付ける"""
    if os.path.exists(WORK):
        subprocess.run(['cmd', '/c', 'rmdir', '/s', '/q', WORK], capture_output=True)
    os.makedirs(WORK)
    s_ini, m_ini = os.path.join(WORK, 's.ini'), os.path.join(WORK, 'm.ini')
    with open(s_ini, 'w', encoding='utf-8') as f:
        f.write(f'[general]\naccept=1\nport={PORT_S}\nkey={KEY}\nlog=1\n')
    with open(m_ini, 'w', encoding='utf-8') as f:
        f.write(f'[general]\naccept=0\nport={PORT_M}\nkey={KEY}\nlog=1\nosd=0\n'
                f'\n[peer]\nhost=127.0.0.1\nport={PORT_PROXY}\nx=1\ny=0\n')
    dest = os.path.join(WORK, 'dest')
    os.makedirs(dest)
    want = len(os.listdir(src))
    procs, paste = [], None
    try:
        procs.append(subprocess.Popen([exe, '-ini', s_ini, '-bind', '127.0.0.1', '-nohook']))
        time.sleep(0.4)
        procs.append(subprocess.Popen([exe, '-ini', m_ini, '-nohook']))
        if not wait_log(os.path.join(WORK, 'm.log'), 'につながりました', 10):
            raise SystemExit('つながらない')
        ps(f"Set-Clipboard -Path '{src}'")
        time.sleep(0.3)
        if reverse:
            subprocess.run([exe, '-ini', s_ini, '-clipsend', '0'])     # つながっているマスターへ
        else:
            subprocess.run([exe, '-ini', m_ini, '-clipsend', '1'])
        for _ in range(200):
            if clip_has_virtual_files():
                break
            time.sleep(0.05)
        else:
            raise SystemExit('受け取った側が一覧をクリップボードに置かない')
        time.sleep(0.5)
        t0 = time.perf_counter()
        paste = subprocess.Popen([FXPASTE, dest], stdout=subprocess.DEVNULL)
        out = os.path.join(dest, os.path.basename(src))
        while True:
            have = os.listdir(out) if os.path.isdir(out) else []
            if len(have) == want and sum(os.path.getsize(os.path.join(out, f)) for f in have) == total:
                break
            if time.perf_counter() - t0 > 590:
                raise SystemExit(f'終わらない({len(have)} / {want} 個)')
            time.sleep(0.02)
        dt = time.perf_counter() - t0
        ok, bad = same_tree(src, out)
        print(f'  {label}: {dt:.1f} 秒  1 ファイル {dt * 1000 / want:.1f} ms  中身 {total * 8 / dt / 1e6:.2f} Mbps  '
              f'{"中身は一致" if ok else "中身が違う: " + bad}', flush=True)
    finally:
        if paste:
            paste.kill()
        for ini in (m_ini, s_ini):
            subprocess.run([exe, '-ini', ini, '-exit'], capture_output=True)
        time.sleep(0.5)
        for p in procs:
            if p.poll() is None:
                p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--files', type=int, default=1000)
    ap.add_argument('--big', type=int, default=0, help='2MB のファイル(乱数とテキストを交互)を N 個')
    ap.add_argument('--rtt', type=float, default=1)
    ap.add_argument('--mbps', type=float, default=0)
    ap.add_argument('--old', help='比べる古い版の input-mouser.exe の置き場所')
    ap.add_argument('--reverse', action='store_true', help='逆向き(受け手でコピーして操作する側で貼り付ける)も測る')
    ap.add_argument('--repeat', type=int, default=1, help='古い版・この版を交互に N 回(ばらつきを見る)')
    a = ap.parse_args()

    ctypes.windll.winmm.timeBeginPeriod(1)
    if not os.path.exists(FXPASTE):
        subprocess.run(['gcc', '-O2', '-municode', '-o', FXPASTE, os.path.join(ROOT, 'tools', 'fxpaste.c'),
                        '-lole32', '-lshell32', '-luuid', '-luser32'], check=True)
    d, total = make_files(a.big or a.files, big=a.big > 0)
    saved = ps('Get-Clipboard -Raw')
    start_proxy(a.mbps, a.rtt)
    print(f'{"2MB のファイル" if a.big else "細かいファイル"} {a.big or a.files} 個(計 {total / 1024:.0f} KB)、'
          f'回線 RTT {a.rtt:g}ms・{a.mbps:g}Mbps(0 = 絞らない)', flush=True)
    try:
        for _ in range(a.repeat):
            if a.old:
                run('古い版どうし', os.path.join(a.old, 'input-mouser.exe'), d, total)
            run('この版どうし', EXE, d, total)
            if a.reverse:
                run('この版どうし(受け手 → 操作する側)', EXE, d, total, True)
    finally:
        if saved.strip():
            p = os.path.join(ROOT, 'build', 'clip-saved.txt')
            with open(p, 'w', encoding='utf-8') as f:
                f.write(saved.rstrip('\n'))
            ps(f"Get-Content -Raw -Encoding utf8 '{p}' | Set-Clipboard")
            os.remove(p)


if __name__ == '__main__':
    main()
