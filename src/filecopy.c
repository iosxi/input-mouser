/* ==================================================================
 * filecopy.c - ファイルのコピー＆貼り付け(貼り付けたときに中身を送る)
 *
 *  コピーした側(中身を持っている側)
 *      エクスプローラーでコピーしたファイル(CF_HDROP)があるまま相手へ移ると、
 *      ファイルの一覧(名前・大きさ・日時。フォルダは中身まで)だけを M_FILES で送る。
 *      中身はまだ送らない。一覧は「申し出」として覚えておき(最新 4 つ)、
 *      相手から M_FREAD(申し出・何番目・位置・長さ)が来たら、読み出して
 *      M_FDATA で返す。読み出しは専用のスレッド(通信を止めないため)。
 *
 *  貼り付ける側
 *      受け取った一覧を「中身はあとから届くファイル」としてクリップボードに置く
 *      (CFSTR_FILEDESCRIPTORW と CFSTR_FILECONTENTS。リモート デスクトップの
 *      ファイル コピーと同じ仕組み)。エクスプローラーで貼り付けると、Windows の
 *      ふつうのコピーの画面が出て、ファイルごとに IStream を読みに来る。
 *      Read が来たら M_FREAD を送り、M_FDATA を待って返す(先読みあり)。
 *      クリップボードの持ち主は専用の STA スレッド(COM の呼び出しはそこへ来る。
 *      読み出しを待つ間も、トレイや設定画面は止まらない)。
 *
 *  中身はファイル用の 2 本目の接続で運ぶ(net.c)。入力の接続を詰まらせないため。
 *  申し出は、送った相手の接続からの読み出しにしか応えない。
 *
 *  まとめ読みと圧縮(v13 から。iiv v8 と同じ仕組み)
 *      エクスプローラーはファイルを 1 つずつ順に読むので、1 つ読むたびに頼んで待つと、
 *      ファイル 1 つにつき 1 往復かかる。小さいファイル(PF_FILE_MAX 以下)は、最初の 1 つを
 *      読みに来たときに、続く小さいファイルを M_FREADMANY でまとめて(1 束 PF_BATCH ほど、
 *      PF_BATCHES 束・PF_AHEAD まで先に)頼み、届いた中身を手元に置いて、読みに来たら渡す。
 *      中身は zlite(deflate の level 1)で圧縮して M_FDATAZ で返す。1 割以上縮まなければ圧縮しない。
 *      圧縮は暗号化(crypto.c)の前にかける(暗号文は縮まない)。大きいファイルは今までどおり
 *      512KB ずつ読むが、M_FREAD に FCF_DEFLATE を付けて圧縮して返してもらう。
 *      受け取る 1 回の大きさは「クリップボードの上限 + 1MB」まで(net.c。上限の最小は 1MB)なので、
 *      1 束の中身は PF_RAW_MAX(1.5MB)までにする(iiv は 4MB)。
 *
 *      M_FREAD      [u32 reqid][u64 申し出][u32 何番目][u64 位置][u32 長さ][u8 FCF_*]
 *      M_FDATA      [u32 reqid][u32 status][中身]
 *      M_FREADMANY  [u32 reqid][u64 申し出][u8 FCF_*][u32 数][u32 何番目 × 数]
 *      M_FDATAZ     [u32 reqid][u32 status][u8 0 = そのまま、1 = deflate][u32 元の長さ][中身]
 *                   M_FREADMANY の中身(展開後)は、頼んだ順に [u32 status][u32 長さ][中身]
 * ================================================================== */

#define COBJMACROS
#include "mouser.h"
#include "zlite.h"
#include <shlobj.h>
#include <shlwapi.h>

#define FC_MAX_ENTRIES 20000
#define FC_CHUNK       (512 * 1024)     /* 1 回の読み出し */
#define FC_WINDOW      6                /* 先に頼んでおく数(3MB) */
#define FC_WAIT_MS     30000
#define OFFER_KEEP     4
#define SLOT_MAX       128

#define PF_FILE_MAX    (256 * 1024)     /* まとめ読みにするファイルの大きさの上限 */
#define PF_BATCH       (1024 * 1024)    /* 1 束の目安 */
#define PF_BATCH_N     1024             /* 1 束のファイルの数の上限 */
#define PF_BATCHES     4                /* 先に頼んでおく束の数 */
#define PF_AHEAD       (8 << 20)        /* 頼んだ分と手元に置いた分の合計の上限 */
#define PF_RAW_MAX     (1536 * 1024)    /* 送る側: 1 束の中身の上限(超えた分は status で断る) */
#define PF_MANY_MAX    4096             /* 送る側: 1 回に受け付けるファイルの数 */
#define FCF_DEFLATE    1u               /* M_FREAD・M_FREADMANY の印: 圧縮して返してよい */
#define Z_LEVEL        1
#define Z_MIN          1024             /* これより短い中身は圧縮しない */

enum { FE_DIR = 1 };

static UINT64 le64(const BYTE *p) { return (UINT64)le32p(p) | ((UINT64)le32p(p + 4) << 32); }

/* ------------------------------------------------------------------ */
/*  小物                                                                */
/* ------------------------------------------------------------------ */

typedef struct { BYTE *p; size_t len, cap; BOOL err; } Bb;

static void bb_put(Bb *b, const void *d, size_t n)
{
    if (b->err) return;
    if (b->len + n > b->cap) {
        size_t nc = max(b->cap * 2, b->len + n + 4096);
        BYTE  *np = b->p ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, b->p, nc) : (BYTE *)HeapAlloc(GetProcessHeap(), 0, nc);
        if (!np) { b->err = TRUE; return; }
        b->p = np; b->cap = nc;
    }
    CopyMemory(b->p + b->len, d, n);
    b->len += n;
}
static void bb_u8(Bb *b, BYTE v)    { bb_put(b, &v, 1); }
static void bb_u16(Bb *b, UINT16 v) { BYTE t[2] = { (BYTE)v, (BYTE)(v >> 8) }; bb_put(b, t, 2); }
static void bb_u32(Bb *b, UINT32 v) { BYTE t[4]; put32p(t, v); bb_put(b, t, 4); }
static void bb_u64(Bb *b, UINT64 v) { bb_u32(b, (UINT32)v); bb_u32(b, (UINT32)(v >> 32)); }

/* ------------------------------------------------------------------ */
/*  コピーした側: 申し出                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    UINT64  id;
    int     mainConn;
    int     n;
    size_t *off;        /* 各ファイルのフルパスの pool 内の位置。フォルダは (size_t)-1 */
    WCHAR  *pool;
} Offer;

static Offer   g_offers[OFFER_KEEP];
static int     g_offerNext;
static SRWLOCK g_offerLock = SRWLOCK_INIT;

typedef struct {
    Bb      msg;
    Bb      pool;       /* WCHAR のフルパスを 0 区切りで */
    size_t *off;
    int     n, cap, skipped;
} Build;

static void build_add(Build *x, const WCHAR *full, const WCHAR *rel, DWORD attr, UINT64 size, FILETIME mt)
{
    int    rl = lstrlenW(rel);
    BOOL   dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    UINT64 mtime = ((UINT64)mt.dwHighDateTime << 32) | mt.dwLowDateTime;

    if (x->n >= FC_MAX_ENTRIES || rl >= MAX_PATH) { x->skipped++; return; }
    if (x->n == x->cap) {
        int     nc = x->cap ? x->cap * 2 : 256;
        size_t *no = x->off ? (size_t *)HeapReAlloc(GetProcessHeap(), 0, x->off, nc * sizeof(size_t))
                            : (size_t *)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(size_t));
        if (!no) { x->msg.err = TRUE; return; }
        x->off = no; x->cap = nc;
    }
    if (dir) x->off[x->n] = (size_t)-1;
    else {
        x->off[x->n] = x->pool.len / sizeof(WCHAR);
        bb_put(&x->pool, full, (lstrlenW(full) + 1) * sizeof(WCHAR));
    }
    x->n++;
    bb_u8(&x->msg, dir ? FE_DIR : 0);
    bb_u32(&x->msg, attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE |
                            FILE_ATTRIBUTE_DIRECTORY));
    bb_u64(&x->msg, dir ? 0 : size);
    bb_u64(&x->msg, mtime);
    bb_u16(&x->msg, (UINT16)rl);
    bb_put(&x->msg, rel, rl * sizeof(WCHAR));
}

/* フォルダの中を順に。リパース ポイント(ジャンクションなど)はたどらない */
static void build_dir(Build *x, const WCHAR *full, const WCHAR *rel, int depth)
{
    WCHAR            pat[MAX_PATH * 2 + 4], cf[MAX_PATH * 2 + 4], cr[MAX_PATH * 2 + 4];
    WIN32_FIND_DATAW fd;
    HANDLE           h;

    if (depth > 64 || lstrlenW(full) + 3 >= (int)ARRAYSIZE(pat)) { x->skipped++; return; }
    wsprintfW(pat, L"%s\\*", full);
    h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { x->skipped++; continue; }
        if (lstrlenW(full) + lstrlenW(fd.cFileName) + 2 >= (int)ARRAYSIZE(cf) ||
            lstrlenW(rel) + lstrlenW(fd.cFileName) + 2 >= (int)ARRAYSIZE(cr)) { x->skipped++; continue; }
        wsprintfW(cf, L"%s\\%s", full, fd.cFileName);
        wsprintfW(cr, L"%s\\%s", rel, fd.cFileName);
        build_add(x, cf, cr, fd.dwFileAttributes,
                  ((UINT64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, fd.ftLastWriteTime);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) build_dir(x, cf, cr, depth + 1);
    } while (FindNextFileW(h, &fd) && x->n < FC_MAX_ENTRIES);
    FindClose(h);
}

/* CF_HDROP から申し出を作る(UI スレッド。クリップボードを開いたまま呼ぶ)。
   送る中身(M_FILES)を *out に。ファイルが無ければ FALSE */
BOOL filecopy_make_offer(int mainConn, HDROP hd, BYTE **out, int *outLen)
{
    Build  x;
    UINT   i, cnt = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
    UINT64 id;
    Offer  o;

    if (!cnt) return FALSE;
    ZeroMemory(&x, sizeof(x));
    crypto_random(&id, sizeof(id));
    bb_u64(&x.msg, id);
    bb_u32(&x.msg, 0);                      /* 数は後で入れる */
    for (i = 0; i < cnt; i++) {
        WCHAR full[MAX_PATH * 2];
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!DragQueryFileW(hd, i, full, ARRAYSIZE(full))) continue;
        if (!GetFileAttributesExW(full, GetFileExInfoStandard, &a)) { x.skipped++; continue; }
        build_add(&x, full, PathFindFileNameW(full), a.dwFileAttributes,
                  ((UINT64)a.nFileSizeHigh << 32) | a.nFileSizeLow, a.ftLastWriteTime);
        if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) build_dir(&x, full, PathFindFileNameW(full), 0);
    }
    if (x.msg.err || x.pool.err || !x.n) {
        if (x.msg.p) HeapFree(GetProcessHeap(), 0, x.msg.p);
        if (x.pool.p) HeapFree(GetProcessHeap(), 0, x.pool.p);
        if (x.off) HeapFree(GetProcessHeap(), 0, x.off);
        return FALSE;
    }
    put32p(x.msg.p + 8, (UINT32)x.n);

    o.id = id; o.mainConn = mainConn; o.n = x.n; o.off = x.off; o.pool = (WCHAR *)x.pool.p;
    AcquireSRWLockExclusive(&g_offerLock);
    {
        Offer *old = &g_offers[g_offerNext];
        if (old->off) HeapFree(GetProcessHeap(), 0, old->off);
        if (old->pool) HeapFree(GetProcessHeap(), 0, old->pool);
        *old = o;
        g_offerNext = (g_offerNext + 1) % OFFER_KEEP;
    }
    ReleaseSRWLockExclusive(&g_offerLock);

    log_printf(L"コピーしたファイル %d 件の一覧を送ります(中身は貼り付けたときに送る%s)", x.n,
               x.skipped ? L"。送れないものは省いた" : L"");
    if (x.skipped) log_printf(L"省いたもの %d 件(パスが長すぎる、リンク、%d 件を超えた など)", x.skipped, FC_MAX_ENTRIES);
    *out    = x.msg.p;
    *outLen = (int)x.msg.len;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  コピーした側: 読み出しに応える(専用スレッド)                        */
/* ------------------------------------------------------------------ */

typedef struct Req {
    struct Req *next;
    int     fileConn, mainConn;
    UINT32  reqid, index, len;
    UINT64  offer, offset;
    BYTE    flags;          /* FCF_* */
    UINT32  count;          /* M_FREADMANY: 何番目の数(0 = M_FREAD) */
    UINT32  many[1];        /* M_FREADMANY: 何番目 × count */
} Req;

static SRWLOCK g_reqLock = SRWLOCK_INIT;
static Req    *g_reqHead, *g_reqTail;
static HANDLE  g_reqEvent, g_srvThread;
static INIT_ONCE g_srvOnce = INIT_ONCE_STATIC_INIT;
static volatile LONG g_srvStop;
static UINT64  g_served, g_servedReqs, g_servedWire, g_servedFiles;

/* 申し出の index 番目のフルパス。別の相手への申し出・古い申し出・フォルダは status を返す */
static DWORD offer_path(UINT64 offer, int mainConn, UINT32 index, WCHAR *path, int cch)
{
    DWORD status = ERROR_FILE_NOT_FOUND;        /* 古くなった申し出 */
    int   i;
    path[0] = 0;
    AcquireSRWLockShared(&g_offerLock);
    for (i = 0; i < OFFER_KEEP; i++) {
        const Offer *o = &g_offers[i];
        if (o->off && o->id == offer) {
            if (o->mainConn != mainConn) status = ERROR_ACCESS_DENIED;      /* 別の相手への申し出 */
            else if (index >= (UINT32)o->n || o->off[index] == (size_t)-1) status = ERROR_INVALID_PARAMETER;
            else { lstrcpynW(path, o->pool + o->off[index], cch); status = 0; }
            break;
        }
    }
    ReleaseSRWLockShared(&g_offerLock);
    return status;
}

static HANDLE open_read(const WCHAR *path)
{
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}

/* 返事を送る。圧縮してよく、1 割以上縮むなら M_FDATAZ(deflate)で。
   まとめ読みの返事(many)はいつも M_FDATAZ、M_FREAD の返事は縮まなければ今までの M_FDATA で */
static void send_reply(ZDWork *zw, int fileConn, UINT32 reqid, DWORD status, const BYTE *data, UINT32 len, BYTE flags, BOOL many)
{
    BYTE  *out = NULL;
    size_t zl = 0;
    if (zw && (flags & FCF_DEFLATE) && len >= Z_MIN) {
        out = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 13 + zd_bound(len));
        if (out) {
            zl = zd_compress(zw, data, 0, len, out + 13, Z_LEVEL, 1);
            if (zl >= (size_t)len - len / 10) zl = 0;
        }
    }
    if (zl || many) {
        if (!zl) {
            if (out) HeapFree(GetProcessHeap(), 0, out);
            out = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 13 + (size_t)len);
            if (!out) return;
            if (len) CopyMemory(out + 13, data, len);
        }
        put32p(out, reqid);
        put32p(out + 4, status);
        out[8] = zl ? 1 : 0;
        put32p(out + 9, len);
        g_servedWire += 13 + (zl ? zl : len);
        net_send_owned(fileConn, M_FDATAZ, out, 13 + (int)(zl ? zl : len));      /* out は net が解放する */
    } else {
        if (out) HeapFree(GetProcessHeap(), 0, out);
        out = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 8 + (size_t)len);
        if (!out) return;
        put32p(out, reqid);
        put32p(out + 4, status);
        if (len) CopyMemory(out + 8, data, len);
        g_servedWire += 8 + len;
        net_send_owned(fileConn, M_FDATA, out, 8 + (int)len);
    }
    g_served += len;
    g_servedReqs++;
}

/* M_FREADMANY: 頼まれた小さいファイルを丸ごと読み、[status][長さ][中身] を並べて返す */
static void serve_many(ZDWork *zw, const Req *r)
{
    Bb     b;
    UINT32 i;
    BYTE  *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, PF_FILE_MAX);
    ZeroMemory(&b, sizeof(b));
    for (i = 0; i < r->count && buf; i++) {
        WCHAR  path[MAX_PATH * 2];
        DWORD  status = offer_path(r->offer, r->mainConn, r->many[i], path, ARRAYSIZE(path)), got = 0;
        if (!status && b.len >= PF_RAW_MAX) status = ERROR_MORE_DATA;           /* 残りは 1 つずつ読んでもらう */
        if (!status) {
            HANDLE f = open_read(path);
            LARGE_INTEGER sz;
            if (f == INVALID_HANDLE_VALUE) status = GetLastError();
            else {
                if (!GetFileSizeEx(f, &sz)) status = GetLastError();
                else if (sz.QuadPart > PF_FILE_MAX) status = ERROR_FILE_TOO_LARGE;  /* 大きくなった: 1 つずつ */
                else if (!ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL)) status = GetLastError();
                CloseHandle(f);
            }
            if (status) { got = 0; log_printf(L"ファイルを読めない %s (%lu)", path, status); }
        }
        bb_u32(&b, status);
        bb_u32(&b, got);
        if (got) bb_put(&b, buf, got);
        if (!status) g_servedFiles++;
    }
    if (buf && !b.err) send_reply(zw, r->fileConn, r->reqid, 0, b.p, (UINT32)b.len, r->flags, TRUE);
    else send_reply(NULL, r->fileConn, r->reqid, ERROR_NOT_ENOUGH_MEMORY, NULL, 0, 0, TRUE);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    if (b.p) HeapFree(GetProcessHeap(), 0, b.p);
}

static DWORD WINAPI server_thread(LPVOID arg)
{
    HANDLE  f = INVALID_HANDLE_VALUE;
    WCHAR   cached[MAX_PATH * 2] = L"";
    ZDWork *zw = zd_work_new();
    (void)arg;

    while (!g_srvStop) {
        Req *r;
        if (WaitForSingleObject(g_reqEvent, 5000) == WAIT_TIMEOUT) {
            if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); f = INVALID_HANDLE_VALUE; cached[0] = 0; }
            if (g_servedReqs) {
                log_printf(L"ファイルの中身を送りました(%I64u 回、%I64u KB、送った量 %I64u KB、まとめ読み %I64u 件)",
                           g_servedReqs, g_served / 1024, g_servedWire / 1024, g_servedFiles);
                g_servedReqs = g_served = g_servedWire = g_servedFiles = 0;
            }
            continue;
        }
        for (;;) {
            WCHAR  path[MAX_PATH * 2];
            DWORD  status, got = 0;
            BYTE  *buf;
            AcquireSRWLockExclusive(&g_reqLock);
            r = g_reqHead;
            if (r) { g_reqHead = r->next; if (!g_reqHead) g_reqTail = NULL; }
            ReleaseSRWLockExclusive(&g_reqLock);
            if (!r) break;
            if (r->count) {
                serve_many(zw, r);
                HeapFree(GetProcessHeap(), 0, r);
                continue;
            }

            status = offer_path(r->offer, r->mainConn, r->index, path, ARRAYSIZE(path));
            if (r->len > FC_CHUNK) r->len = FC_CHUNK;
            buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, status ? 1 : r->len);
            if (!buf) { HeapFree(GetProcessHeap(), 0, r); continue; }
            if (!status) {
                if (lstrcmpiW(path, cached)) {
                    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
                    f = open_read(path);
                    lstrcpynW(cached, f != INVALID_HANDLE_VALUE ? path : L"", ARRAYSIZE(cached));
                }
                if (f == INVALID_HANDLE_VALUE) status = GetLastError();
                else {
                    LARGE_INTEGER li;
                    li.QuadPart = (LONGLONG)r->offset;
                    if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN) || !ReadFile(f, buf, r->len, &got, NULL))
                        status = GetLastError();
                }
                if (status) got = 0;
            }
            send_reply(zw, r->fileConn, r->reqid, status, buf, got, r->flags, FALSE);
            HeapFree(GetProcessHeap(), 0, buf);
            HeapFree(GetProcessHeap(), 0, r);
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (zw) zd_work_free(zw);
    return 0;
}

static BOOL CALLBACK srv_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    g_reqEvent  = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_srvThread = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    return g_srvThread != NULL;
}

static void req_push(Req *r)
{
    AcquireSRWLockExclusive(&g_reqLock);
    if (g_reqTail) g_reqTail->next = r; else g_reqHead = r;
    g_reqTail = r;
    ReleaseSRWLockExclusive(&g_reqLock);
    SetEvent(g_reqEvent);
}

/* net スレッドから: [u32 reqid][u64 申し出][u32 何番目][u64 位置][u32 長さ]([u8 FCF_*]) */
void filecopy_request(int fileConn, int mainConn, const BYTE *p, int n)
{
    Req *r;
    if (n < 28 || !InitOnceExecuteOnce(&g_srvOnce, srv_init, NULL, NULL)) return;
    r = (Req *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Req));
    if (!r) return;
    r->fileConn = fileConn;
    r->mainConn = mainConn;
    r->reqid    = le32p(p);
    r->offer    = le64(p + 4);
    r->index    = le32p(p + 12);
    r->offset   = le64(p + 16);
    r->len      = le32p(p + 24);
    r->flags    = n > 28 ? p[28] : 0;
    req_push(r);
}

/* net スレッドから: [u32 reqid][u64 申し出][u8 FCF_*][u32 数][u32 何番目 × 数] */
void filecopy_request_many(int fileConn, int mainConn, const BYTE *p, int n)
{
    Req   *r;
    UINT32 cnt, i;
    if (n < 17 || !InitOnceExecuteOnce(&g_srvOnce, srv_init, NULL, NULL)) return;
    cnt = le32p(p + 13);
    if (!cnt || cnt > PF_MANY_MAX || (UINT32)(n - 17) < cnt * 4) return;
    r = (Req *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Req) + cnt * sizeof(UINT32));
    if (!r) return;
    r->fileConn = fileConn;
    r->mainConn = mainConn;
    r->reqid    = le32p(p);
    r->offer    = le64(p + 4);
    r->flags    = p[12];
    r->count    = cnt;
    for (i = 0; i < cnt; i++) r->many[i] = le32p(p + 17 + i * 4);
    req_push(r);
}

/* ------------------------------------------------------------------ */
/*  貼り付ける側: 読み出しの待ち合わせ                                  */
/* ------------------------------------------------------------------ */

enum { SL_FREE, SL_WAIT, SL_DONE, SL_ABANDON };

typedef struct {
    int     state;
    int     conn;
    UINT32  reqid;
    HANDLE  ev;
    BYTE   *data;
    UINT32  len;
    DWORD   status;
    BYTE    codec;          /* 0 = そのまま、1 = deflate(rawLen に展開する) */
    UINT32  rawLen;
} Slot;

static Slot    g_slots[SLOT_MAX];
static SRWLOCK g_slotLock = SRWLOCK_INIT;
static volatile LONG g_nextReq;

static void slot_release_locked(Slot *s)
{
    if (s->data) HeapFree(GetProcessHeap(), 0, s->data);
    s->data  = NULL;
    s->state = SL_FREE;
}

static int slot_new(int conn, UINT32 *reqid)
{
    int i;
    AcquireSRWLockExclusive(&g_slotLock);
    for (i = 0; i < SLOT_MAX && g_slots[i].state != SL_FREE; i++) ;
    if (i < SLOT_MAX) {
        Slot *s = &g_slots[i];
        if (!s->ev) s->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        ResetEvent(s->ev);
        s->state  = SL_WAIT;
        s->conn   = conn;
        s->reqid  = *reqid = (UINT32)InterlockedIncrement(&g_nextReq);
        s->data   = NULL;
        s->len    = 0;
        s->status = 0;
        s->codec  = 0;
        s->rawLen = 0;
    } else i = -1;
    ReleaseSRWLockExclusive(&g_slotLock);
    return i;
}

static void slot_abandon(int i)
{
    AcquireSRWLockExclusive(&g_slotLock);
    if (g_slots[i].state == SL_WAIT) g_slots[i].state = SL_ABANDON;     /* 届いたら捨てる */
    else slot_release_locked(&g_slots[i]);
    ReleaseSRWLockExclusive(&g_slotLock);
}

static void slot_fill(int fileConn, UINT32 reqid, DWORD status, BYTE codec, UINT32 rawLen, const BYTE *p, int n)
{
    int i;
    AcquireSRWLockExclusive(&g_slotLock);
    for (i = 0; i < SLOT_MAX; i++) {
        Slot *s = &g_slots[i];
        if (s->reqid != reqid || s->conn != fileConn || (s->state != SL_WAIT && s->state != SL_ABANDON)) continue;
        if (s->state == SL_ABANDON) { slot_release_locked(s); break; }
        s->status = status;
        s->codec  = codec;
        s->rawLen = rawLen;
        s->len    = (UINT32)n;
        if (s->len) {
            s->data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, s->len);
            if (s->data) CopyMemory(s->data, p, s->len);
            else { s->status = ERROR_NOT_ENOUGH_MEMORY; s->len = 0; }
        }
        s->state = SL_DONE;
        SetEvent(s->ev);
        break;
    }
    ReleaseSRWLockExclusive(&g_slotLock);
}

/* net スレッドから: [u32 reqid][u32 status][中身] */
void filecopy_deliver(int fileConn, const BYTE *p, int n)
{
    if (n < 8) return;
    slot_fill(fileConn, le32p(p), le32p(p + 4), 0, (UINT32)(n - 8), p + 8, n - 8);
}

/* net スレッドから: [u32 reqid][u32 status][u8 方式][u32 元の長さ][中身]。展開は待っている側でする */
void filecopy_deliver_z(int fileConn, const BYTE *p, int n)
{
    UINT32 raw;
    BYTE   codec;
    if (n < 13) return;
    codec = p[8];
    raw   = le32p(p + 9);
    if (codec > 1 || raw > (64u << 20) || (!codec && raw != (UINT32)(n - 13)))
        slot_fill(fileConn, le32p(p), ERROR_INVALID_DATA, 0, 0, NULL, 0);
    else slot_fill(fileConn, le32p(p), le32p(p + 4), codec, raw, p + 13, n - 13);
}

/* 届いた中身を受け取る(展開はしない)。slot の data は呼び手のものになる。
   g_slotLock を持って呼ぶ。失敗していれば status を返す */
typedef struct { BYTE *data; UINT32 len, rawLen; BYTE codec; } Got;

static DWORD slot_take_locked(Slot *s, Got *g)
{
    ZeroMemory(g, sizeof(*g));
    if (s->status) return s->status;
    g->data   = s->data;
    g->len    = s->len;
    g->codec  = s->codec;
    g->rawLen = s->rawLen;
    s->data   = NULL;
    return 0;
}

/* 圧縮されていれば展開する(g_slotLock の外で。数 MB の展開で net スレッドを待たせない) */
static DWORD got_unpack(Got *g, BYTE **data, UINT32 *len)
{
    *data = NULL;
    *len  = 0;
    if (g->codec == 1) {
        BYTE     *out = g->rawLen ? (BYTE *)HeapAlloc(GetProcessHeap(), 0, g->rawLen) : NULL;
        ZInflate *z   = zi_new();
        int       r   = (out && z) ? zi_inflate(z, g->data, g->len, out, g->rawLen) : -3;
        if (z) zi_free(z);
        if (g->data) HeapFree(GetProcessHeap(), 0, g->data);
        g->data = NULL;
        if (r) {
            if (out) HeapFree(GetProcessHeap(), 0, out);
            return r == -3 ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_DATA;
        }
        *data = out;
        *len  = g->rawLen;
        return 0;
    }
    *data   = g->data;
    *len    = g->len;
    g->data = NULL;
    return 0;
}

/* ファイル用の接続が切れた。待っているものはすべて失敗にする */
void filecopy_conn_closed(int fileConn)
{
    int i;
    AcquireSRWLockExclusive(&g_slotLock);
    for (i = 0; i < SLOT_MAX; i++) {
        Slot *s = &g_slots[i];
        if (s->conn != fileConn) continue;
        if (s->state == SL_WAIT) { s->status = ERROR_CONNECTION_ABORTED; s->state = SL_DONE; SetEvent(s->ev); }
        else if (s->state == SL_ABANDON) slot_release_locked(s);
    }
    ReleaseSRWLockExclusive(&g_slotLock);
}

/* ------------------------------------------------------------------ */
/*  貼り付ける側: ファイルの中身(IStream)                               */
/* ------------------------------------------------------------------ */

typedef struct {
    WCHAR  *name;       /* 相対パス */
    UINT64  size, mtime;
    DWORD   attr;
    BOOL    dir;
} FEntry;

enum { PF_NONE, PF_ASKED, PF_HAVE, PF_GONE };     /* GONE = 渡した・断られた(1 つずつ読む) */

typedef struct {
    int     slot;
    UINT32  reqid;
    UINT32  n;
    UINT32 *idx;            /* 頼んだ順の何番目 */
} PfBatch;

typedef struct {
    LONG    ref;
    UINT64  id;
    int     mainConn;
    int     n;
    FEntry *e;
    WCHAR  *pool;
    /* まとめ読み(pf_*)。pf を持って触る */
    CRITICAL_SECTION pf;
    BYTE   *pfState;        /* PF_*(n 個。使い始めるときに作る) */
    BYTE  **pfData;         /* PF_HAVE の中身 */
    int     pfNext;         /* 次に頼むところ */
    int     pfLow;          /* これより前は捨てた */
    PfBatch pfB[PF_BATCHES];
    int     pfNb;           /* 頼んでいる束(古い順) */
    UINT64  pfAhead;        /* PF_ASKED と PF_HAVE の大きさの合計 */
    UINT64  pfGot, pfFiles, pfBatches, pfMiss;
} OfferIn;              /* 受け取った一覧(データ オブジェクトと各ストリームが参照する) */

static void pf_drop_batch(OfferIn *o, int k)
{
    UINT32 i;
    for (i = 0; i < o->pfB[k].n; i++) {
        UINT32 x = o->pfB[k].idx[i];
        if (o->pfState[x] == PF_ASKED) { o->pfState[x] = PF_GONE; o->pfAhead -= o->e[x].size; }
    }
    HeapFree(GetProcessHeap(), 0, o->pfB[k].idx);
    o->pfNb--;
    MoveMemory(&o->pfB[k], &o->pfB[k + 1], (o->pfNb - k) * sizeof(PfBatch));
}

static void offer_in_release(OfferIn *o)
{
    int i;
    if (InterlockedDecrement(&o->ref)) return;
    if (o->pfBatches)
        log_printf(L"まとめ読み: %I64u 束、%I64u 件、届いた量 %I64u KB(1 つずつ読んだもの %I64u 件)",
                   o->pfBatches, o->pfFiles, o->pfGot / 1024, o->pfMiss);
    while (o->pfNb) { slot_abandon(o->pfB[0].slot); pf_drop_batch(o, 0); }
    if (o->pfData) {
        for (i = 0; i < o->n; i++) if (o->pfData[i]) HeapFree(GetProcessHeap(), 0, o->pfData[i]);
        HeapFree(GetProcessHeap(), 0, o->pfData);
    }
    if (o->pfState) HeapFree(GetProcessHeap(), 0, o->pfState);
    DeleteCriticalSection(&o->pf);
    HeapFree(GetProcessHeap(), 0, o->e);
    HeapFree(GetProcessHeap(), 0, o->pool);
    HeapFree(GetProcessHeap(), 0, o);
}

static int file_conn_for(int mainConn)
{
    int fc = net_file_conn(mainConn), i;
    if (fc >= 0) return fc;
    if (mainConn < CONN_IN_BASE) net_file_open(mainConn);   /* こちらが操作する側なら張る */
    for (i = 0; i < 100 && (fc = net_file_conn(mainConn)) < 0; i++) Sleep(50);
    return fc;
}

static BOOL pf_small(const FEntry *e) { return !e->dir && e->size && e->size <= PF_FILE_MAX; }

/* 続く小さいファイルを、束にして頼めるだけ頼む */
static void pf_fill(OfferIn *o)
{
    int fc = file_conn_for(o->mainConn);
    if (fc < 0 || !(net_file_caps(fc) & CAP_FBATCH)) return;
    while (o->pfNb < PF_BATCHES && o->pfAhead < PF_AHEAD) {
        UINT32 *idx = (UINT32 *)HeapAlloc(GetProcessHeap(), 0, PF_BATCH_N * sizeof(UINT32));
        BYTE   *msg;
        UINT32  cnt = 0, reqid, k;
        UINT64  bytes = 0;
        int     i, sl;
        if (!idx) return;
        for (i = o->pfNext; i < o->n && cnt < PF_BATCH_N && bytes < PF_BATCH; i++) {
            if (!pf_small(&o->e[i]) || o->pfState[i] != PF_NONE) continue;
            idx[cnt++] = (UINT32)i;
            bytes += o->e[i].size;
        }
        o->pfNext = i;
        if (!cnt || (sl = slot_new(fc, &reqid)) < 0) { HeapFree(GetProcessHeap(), 0, idx); return; }
        msg = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 17 + cnt * 4);
        if (!msg) {
            slot_abandon(sl);
            HeapFree(GetProcessHeap(), 0, idx);
            return;
        }
        put32p(msg, reqid);
        put32p(msg + 4, (UINT32)o->id); put32p(msg + 8, (UINT32)(o->id >> 32));
        msg[12] = FCF_DEFLATE;
        put32p(msg + 13, cnt);
        for (k = 0; k < cnt; k++) put32p(msg + 17 + k * 4, idx[k]);
        net_send_owned(fc, M_FREADMANY, msg, 17 + (int)cnt * 4);      /* msg は net が解放する */
        for (k = 0; k < cnt; k++) o->pfState[idx[k]] = PF_ASKED;
        o->pfAhead += bytes;
        o->pfB[o->pfNb].slot  = sl;
        o->pfB[o->pfNb].reqid = reqid;
        o->pfB[o->pfNb].n     = cnt;
        o->pfB[o->pfNb].idx   = idx;
        o->pfNb++;
        o->pfBatches++;
    }
}

/* いちばん古い束が届いた: 中身を手元に置く。[u32 status][u32 長さ][中身] が頼んだ順に並ぶ */
static void pf_collect(OfferIn *o)
{
    PfBatch *b = &o->pfB[0];
    Slot    *sl = &g_slots[b->slot];
    BYTE    *data = NULL;
    UINT32   len = 0, i, pos = 0;
    DWORD    st;
    Got      g;
    AcquireSRWLockExclusive(&g_slotLock);
    st = slot_take_locked(sl, &g);
    slot_release_locked(sl);
    ReleaseSRWLockExclusive(&g_slotLock);
    if (!st) st = got_unpack(&g, &data, &len);
    if (st) log_printf(L"まとめ読みを受け取れませんでした (%lu)。1 つずつ読みます", st);
    else o->pfGot += len;
    for (i = 0; !st && i < b->n; i++) {
        UINT32 x = b->idx[i], fst, fl;
        if (pos + 8 > len) break;
        fst = le32p(data + pos);
        fl  = le32p(data + pos + 4);
        pos += 8;
        if (fl > len - pos) break;
        if (!fst && fl == o->e[x].size && (int)x >= o->pfLow && o->pfState[x] == PF_ASKED) {
            o->pfData[x] = (BYTE *)HeapAlloc(GetProcessHeap(), 0, fl);
            if (o->pfData[x]) {
                CopyMemory(o->pfData[x], data + pos, fl);
                o->pfState[x] = PF_HAVE;
            }
        }
        pos += fl;
    }
    if (data) HeapFree(GetProcessHeap(), 0, data);
    pf_drop_batch(o, 0);            /* 手元に置けなかったものは PF_GONE(1 つずつ読む) */
}

/* index 番目(小さいファイル)の中身をまとめ読みで得る。得られなければ FALSE(1 つずつ読む) */
static BOOL pf_get(OfferIn *o, int index, BYTE **data, UINT32 *len)
{
    BOOL ok = FALSE;
    int  i;
    if (!pf_small(&o->e[index])) return FALSE;
    EnterCriticalSection(&o->pf);
    if (!o->pfState) {
        o->pfState = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, o->n);
        o->pfData  = (BYTE **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, o->n * sizeof(BYTE *));
        if (!o->pfState || !o->pfData) {
            if (o->pfState) HeapFree(GetProcessHeap(), 0, o->pfState);
            if (o->pfData) HeapFree(GetProcessHeap(), 0, o->pfData);
            o->pfState = NULL;
            o->pfData  = NULL;
            LeaveCriticalSection(&o->pf);
            return FALSE;
        }
    }
    /* 飛ばされた分は捨てる(エクスプローラーは前から順に読む) */
    for (i = o->pfLow; i < index; i++) {
        if (o->pfState[i] != PF_HAVE) continue;
        HeapFree(GetProcessHeap(), 0, o->pfData[i]);
        o->pfData[i]  = NULL;
        o->pfState[i] = PF_GONE;
        o->pfAhead   -= o->e[i].size;
    }
    if (index > o->pfLow) o->pfLow = index;
    for (;;) {
        BYTE st = o->pfState[index];
        if (st == PF_HAVE) {
            *data = o->pfData[index];
            *len  = (UINT32)o->e[index].size;
            o->pfData[index]  = NULL;
            o->pfState[index] = PF_GONE;
            o->pfAhead -= o->e[index].size;
            o->pfFiles++;
            ok = TRUE;
            pf_fill(o);
            break;
        }
        if (st == PF_GONE) { o->pfMiss++; break; }
        if (st == PF_NONE) {
            o->pfNext = index;
            pf_fill(o);
            if (o->pfState[index] == PF_NONE) { o->pfState[index] = PF_GONE; continue; }
        }
        if (!o->pfNb) { o->pfState[index] = PF_GONE; continue; }      /* 起きないはず */
        {
            /* いちばん古い束を待つ(外で待ち、戻ったら同じ束か確かめる) */
            int    sl = o->pfB[0].slot;
            UINT32 rq = o->pfB[0].reqid;
            DWORD  w;
            LeaveCriticalSection(&o->pf);
            w = WaitForSingleObject(g_slots[sl].ev, FC_WAIT_MS);
            EnterCriticalSection(&o->pf);
            if (!o->pfNb || o->pfB[0].reqid != rq) continue;               /* ほかのスレッドが受け取った */
            if (w == WAIT_OBJECT_0) pf_collect(o);
            else {
                log_printf(L"まとめ読みの返事が来ません。1 つずつ読みます");
                slot_abandon(sl);
                pf_drop_batch(o, 0);
            }
        }
    }
    LeaveCriticalSection(&o->pf);
    return ok;
}

typedef struct {
    int    slot;
    UINT64 offset;
    UINT32 len;
} Pend;

typedef struct {
    IStream  iface;
    LONG     ref;
    OfferIn *o;
    int      index;
    UINT64   size, pos, reqPos;
    Pend     q[FC_WINDOW];
    int      qh, qn;
    BYTE    *cur;
    UINT64   curStart;
    UINT32   curLen;
    HRESULT  err;
    BOOL     pfTried;       /* まとめ読みを試した */
} VStream;

#define VS(p) ((VStream *)(p))

static void vs_drop_pending(VStream *s)
{
    while (s->qn) { slot_abandon(s->q[s->qh].slot); s->qh = (s->qh + 1) % FC_WINDOW; s->qn--; }
    s->qh = 0;
}

static BOOL vs_request(VStream *s)
{
    BYTE   b[29];
    UINT32 reqid, len;
    int    fc, sl;
    if (s->qn >= FC_WINDOW || s->reqPos >= s->size) return FALSE;
    fc = file_conn_for(s->o->mainConn);
    if (fc < 0) { s->err = HRESULT_FROM_WIN32(ERROR_CONNECTION_UNAVAIL); return FALSE; }
    sl = slot_new(fc, &reqid);
    if (sl < 0) return FALSE;
    len = (UINT32)min((UINT64)FC_CHUNK, s->size - s->reqPos);
    put32p(b, reqid);
    put32p(b + 4, (UINT32)s->o->id); put32p(b + 8, (UINT32)(s->o->id >> 32));
    put32p(b + 12, (UINT32)s->index);
    put32p(b + 16, (UINT32)s->reqPos); put32p(b + 20, (UINT32)(s->reqPos >> 32));
    put32p(b + 24, len);
    b[28] = (net_file_caps(fc) & CAP_FBATCH) ? FCF_DEFLATE : 0;
    net_send(fc, M_FREAD, b, sizeof(b));
    s->q[(s->qh + s->qn) % FC_WINDOW].slot   = sl;
    s->q[(s->qh + s->qn) % FC_WINDOW].offset = s->reqPos;
    s->q[(s->qh + s->qn) % FC_WINDOW].len    = len;
    s->qn++;
    s->reqPos += len;
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE vs_QueryInterface(IStream *p, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStream) || IsEqualIID(riid, &IID_ISequentialStream)) {
        *out = p;
        InterlockedIncrement(&VS(p)->ref);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE vs_AddRef(IStream *p) { return (ULONG)InterlockedIncrement(&VS(p)->ref); }
static ULONG STDMETHODCALLTYPE vs_Release(IStream *p)
{
    VStream *s = VS(p);
    LONG r = InterlockedDecrement(&s->ref);
    if (r) return (ULONG)r;
    vs_drop_pending(s);
    if (s->cur) HeapFree(GetProcessHeap(), 0, s->cur);
    if (s->pos >= s->size && s->size)
        log_printf(L"貼り付け: %s を受け取りました(%I64u KB)", s->o->e[s->index].name, s->size / 1024);
    offer_in_release(s->o);
    HeapFree(GetProcessHeap(), 0, s);
    return 0;
}

static HRESULT STDMETHODCALLTYPE vs_Read(IStream *p, void *dst, ULONG cb, ULONG *pcb)
{
    VStream *s = VS(p);
    ULONG    total = 0;

    /* 小さいファイルは、まとめ読みで手元に届いた中身を丸ごと使う */
    if (!s->pfTried && !s->cur && !s->qn && s->pos < s->size) {
        BYTE  *d;
        UINT32 n;
        s->pfTried = TRUE;
        if (pf_get(s->o, s->index, &d, &n)) {
            s->cur      = d;
            s->curStart = 0;
            s->curLen   = n;
        }
    }
    while (total < cb && s->pos < s->size) {
        if (s->cur && s->pos >= s->curStart && s->pos < s->curStart + s->curLen) {
            ULONG k = (ULONG)min((UINT64)(cb - total), s->curStart + s->curLen - s->pos);
            CopyMemory((BYTE *)dst + total, s->cur + (s->pos - s->curStart), k);
            total  += k;
            s->pos += k;
            continue;
        }
        if (s->cur) { HeapFree(GetProcessHeap(), 0, s->cur); s->cur = NULL; s->curLen = 0; }
        if (s->err) break;
        /* 先頭の頼みが今の位置からでなければ(Seek の後)頼み直す */
        if (s->qn && s->q[s->qh].offset != s->pos) { vs_drop_pending(s); s->reqPos = s->pos; }
        if (!s->qn) s->reqPos = s->pos;
        while (vs_request(s)) ;
        if (!s->qn) { if (!s->err) s->err = E_FAIL; break; }
        {
            Pend  *h  = &s->q[s->qh];
            Slot  *sl = &g_slots[h->slot];
            DWORD  w  = WaitForSingleObject(sl->ev, FC_WAIT_MS), st = 0;
            BOOL   got = FALSE;
            Got    g;
            AcquireSRWLockExclusive(&g_slotLock);
            if (w != WAIT_OBJECT_0 || sl->state != SL_DONE) {
                if (sl->state == SL_WAIT) sl->state = SL_ABANDON;
                s->err = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            } else {
                st = slot_take_locked(sl, &g);
                slot_release_locked(sl);
                got = TRUE;
            }
            ReleaseSRWLockExclusive(&g_slotLock);
            if (got) {
                if (!st) st = got_unpack(&g, &s->cur, &s->curLen);
                s->curStart = h->offset;
                if (st) s->err = HRESULT_FROM_WIN32(st);
                else if (!s->curLen) s->err = STG_E_READFAULT;     /* 短くなった */
            }
            s->qh = (s->qh + 1) % FC_WINDOW;
            s->qn--;
            if (s->err) {
                log_printf(L"貼り付け: %s を読めませんでした (0x%08lX)", s->o->e[s->index].name, s->err);
                break;
            }
        }
    }
    if (pcb) *pcb = total;
    if (!total && s->err) return s->err;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vs_Write(IStream *p, const void *b, ULONG cb, ULONG *pcb)
{ (void)p; (void)b; (void)cb; if (pcb) *pcb = 0; return STG_E_ACCESSDENIED; }

static HRESULT STDMETHODCALLTYPE vs_Seek(IStream *p, LARGE_INTEGER mv, DWORD origin, ULARGE_INTEGER *np)
{
    VStream *s = VS(p);
    LONGLONG base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? (LONGLONG)s->pos : (LONGLONG)s->size;
    LONGLONG to   = base + mv.QuadPart;
    if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
    if (to < 0) return STG_E_INVALIDFUNCTION;
    s->pos = (UINT64)to;
    if (np) np->QuadPart = s->pos;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vs_SetSize(IStream *p, ULARGE_INTEGER n) { (void)p; (void)n; return STG_E_ACCESSDENIED; }

static HRESULT STDMETHODCALLTYPE vs_CopyTo(IStream *p, IStream *to, ULARGE_INTEGER cb, ULARGE_INTEGER *rd, ULARGE_INTEGER *wr)
{
    BYTE   *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, FC_CHUNK);
    UINT64  left = cb.QuadPart, r = 0, w = 0;
    HRESULT hr = S_OK;
    if (!buf) return E_OUTOFMEMORY;
    while (left) {
        ULONG got = 0, put = 0;
        hr = vs_Read(p, buf, (ULONG)min(left, (UINT64)FC_CHUNK), &got);
        if (FAILED(hr) || !got) break;
        r += got;
        hr = IStream_Write(to, buf, got, &put);
        w += put;
        if (FAILED(hr)) break;
        left -= got;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    if (rd) rd->QuadPart = r;
    if (wr) wr->QuadPart = w;
    return FAILED(hr) ? hr : S_OK;
}

static HRESULT STDMETHODCALLTYPE vs_Commit(IStream *p, DWORD f) { (void)p; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE vs_Revert(IStream *p) { (void)p; return S_OK; }
static HRESULT STDMETHODCALLTYPE vs_Lock(IStream *p, ULARGE_INTEGER o, ULARGE_INTEGER c, DWORD t)
{ (void)p; (void)o; (void)c; (void)t; return STG_E_INVALIDFUNCTION; }

static HRESULT STDMETHODCALLTYPE vs_Stat(IStream *p, STATSTG *st, DWORD flag)
{
    VStream     *s = VS(p);
    const FEntry *e = &s->o->e[s->index];
    ZeroMemory(st, sizeof(*st));
    st->type           = STGTY_STREAM;
    st->cbSize.QuadPart = s->size;
    st->mtime.dwLowDateTime  = (DWORD)e->mtime;
    st->mtime.dwHighDateTime = (DWORD)(e->mtime >> 32);
    st->grfMode        = STGM_READ;
    if (!(flag & STATFLAG_NONAME)) {
        const WCHAR *nm = PathFindFileNameW(e->name);
        SIZE_T       bytes = (lstrlenW(nm) + 1) * sizeof(WCHAR);
        st->pwcsName = (LPOLESTR)CoTaskMemAlloc(bytes);
        if (st->pwcsName) CopyMemory(st->pwcsName, nm, bytes);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vs_Clone(IStream *p, IStream **o) { (void)p; *o = NULL; return E_NOTIMPL; }

static IStreamVtbl g_vsVtbl = {
    vs_QueryInterface, vs_AddRef, vs_Release, vs_Read, vs_Write, vs_Seek, vs_SetSize, vs_CopyTo,
    vs_Commit, vs_Revert, vs_Lock, vs_Lock, vs_Stat, vs_Clone
};

static IStream *vs_new(OfferIn *o, int index)
{
    VStream *s = (VStream *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VStream));
    if (!s) return NULL;
    s->iface.lpVtbl = &g_vsVtbl;
    s->ref   = 1;
    s->o     = o;
    s->index = index;
    s->size  = o->e[index].size;
    InterlockedIncrement(&o->ref);
    return &s->iface;
}

/* ------------------------------------------------------------------ */
/*  貼り付ける側: クリップボードに置くデータ オブジェクト               */
/* ------------------------------------------------------------------ */

typedef struct {
    IDataObject iface;
    LONG        ref;
    OfferIn    *o;
} VData;

#define VD(p) ((VData *)(p))

static CLIPFORMAT g_cfDesc, g_cfContents, g_cfEffect;

static HRESULT STDMETHODCALLTYPE vd_QueryInterface(IDataObject *p, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) {
        *out = p;
        InterlockedIncrement(&VD(p)->ref);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE vd_AddRef(IDataObject *p) { return (ULONG)InterlockedIncrement(&VD(p)->ref); }
static ULONG STDMETHODCALLTYPE vd_Release(IDataObject *p)
{
    VData *d = VD(p);
    LONG   r = InterlockedDecrement(&d->ref);
    if (r) return (ULONG)r;
    offer_in_release(d->o);
    HeapFree(GetProcessHeap(), 0, d);
    return 0;
}

static HGLOBAL make_descriptor(const OfferIn *o)
{
    SIZE_T   bytes = sizeof(UINT) + (SIZE_T)o->n * sizeof(FILEDESCRIPTORW);
    HGLOBAL  g = GlobalAlloc(GHND, bytes);
    FILEGROUPDESCRIPTORW *fg;
    int      i;
    if (!g) return NULL;
    fg = (FILEGROUPDESCRIPTORW *)GlobalLock(g);
    fg->cItems = (UINT)o->n;
    for (i = 0; i < o->n; i++) {
        FILEDESCRIPTORW *f = &fg->fgd[i];
        const FEntry    *e = &o->e[i];
        f->dwFlags          = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
        f->dwFileAttributes = e->dir ? FILE_ATTRIBUTE_DIRECTORY : (e->attr ? e->attr : FILE_ATTRIBUTE_NORMAL);
        f->ftLastWriteTime.dwLowDateTime  = (DWORD)e->mtime;
        f->ftLastWriteTime.dwHighDateTime = (DWORD)(e->mtime >> 32);
        f->nFileSizeLow  = (DWORD)e->size;
        f->nFileSizeHigh = (DWORD)(e->size >> 32);
        lstrcpynW(f->cFileName, e->name, MAX_PATH);
    }
    GlobalUnlock(g);
    return g;
}

static HRESULT STDMETHODCALLTYPE vd_GetData(IDataObject *p, FORMATETC *fe, STGMEDIUM *m)
{
    VData *d = VD(p);
    ZeroMemory(m, sizeof(*m));
    if (fe->cfFormat == g_cfDesc && (fe->tymed & TYMED_HGLOBAL)) {
        m->tymed   = TYMED_HGLOBAL;
        m->hGlobal = make_descriptor(d->o);
        return m->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    if (fe->cfFormat == g_cfContents && (fe->tymed & TYMED_ISTREAM)) {
        if (fe->lindex < 0 || fe->lindex >= d->o->n || d->o->e[fe->lindex].dir) return DV_E_LINDEX;
        m->tymed = TYMED_ISTREAM;
        m->pstm  = vs_new(d->o, fe->lindex);
        return m->pstm ? S_OK : E_OUTOFMEMORY;
    }
    if (fe->cfFormat == g_cfEffect && (fe->tymed & TYMED_HGLOBAL)) {
        HGLOBAL g = GlobalAlloc(GHND, sizeof(DWORD));
        if (!g) return E_OUTOFMEMORY;
        *(DWORD *)GlobalLock(g) = DROPEFFECT_COPY;          /* 貼り付けはいつもコピー */
        GlobalUnlock(g);
        m->tymed   = TYMED_HGLOBAL;
        m->hGlobal = g;
        return S_OK;
    }
    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE vd_GetDataHere(IDataObject *p, FORMATETC *f, STGMEDIUM *m)
{ (void)p; (void)f; (void)m; return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE vd_QueryGetData(IDataObject *p, FORMATETC *fe)
{
    (void)p;
    if (fe->cfFormat == g_cfDesc && (fe->tymed & TYMED_HGLOBAL)) return S_OK;
    if (fe->cfFormat == g_cfContents && (fe->tymed & TYMED_ISTREAM)) return S_OK;
    if (fe->cfFormat == g_cfEffect && (fe->tymed & TYMED_HGLOBAL)) return S_OK;
    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE vd_GetCanonical(IDataObject *p, FORMATETC *in, FORMATETC *out)
{ (void)p; *out = *in; out->ptd = NULL; return DATA_S_SAMEFORMATETC; }

/* エクスプローラーは貼り付けの結果などを書いてくる。受け取って捨てる */
static HRESULT STDMETHODCALLTYPE vd_SetData(IDataObject *p, FORMATETC *f, STGMEDIUM *m, BOOL rel)
{ (void)p; (void)f; if (rel) ReleaseStgMedium(m); return S_OK; }

static HRESULT STDMETHODCALLTYPE vd_EnumFormatEtc(IDataObject *p, DWORD dir, IEnumFORMATETC **e)
{
    FORMATETC f[3] = {
        { 0, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
        { 0, NULL, DVASPECT_CONTENT, -1, TYMED_ISTREAM },
        { 0, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
    };
    (void)p;
    if (dir != DATADIR_GET) { *e = NULL; return E_NOTIMPL; }
    f[0].cfFormat = g_cfDesc;
    f[1].cfFormat = g_cfContents;
    f[2].cfFormat = g_cfEffect;
    return SHCreateStdEnumFmtEtc(3, f, e);
}

static HRESULT STDMETHODCALLTYPE vd_DAdvise(IDataObject *p, FORMATETC *f, DWORD a, IAdviseSink *s, DWORD *c)
{ (void)p; (void)f; (void)a; (void)s; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE vd_DUnadvise(IDataObject *p, DWORD c) { (void)p; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE vd_EnumDAdvise(IDataObject *p, IEnumSTATDATA **e) { (void)p; *e = NULL; return OLE_E_ADVISENOTSUPPORTED; }

static IDataObjectVtbl g_vdVtbl = {
    vd_QueryInterface, vd_AddRef, vd_Release, vd_GetData, vd_GetDataHere, vd_QueryGetData,
    vd_GetCanonical, vd_SetData, vd_EnumFormatEtc, vd_DAdvise, vd_DUnadvise, vd_EnumDAdvise
};

/* ------------------------------------------------------------------ */
/*  貼り付ける側: クリップボードのスレッド                              */
/* ------------------------------------------------------------------ */

#define FM_OFFER (WM_APP + 1)
#define FM_QUIT  (WM_APP + 2)

static DWORD     g_clipTid;
static HANDLE    g_clipThread, g_clipReady;
static INIT_ONCE g_clipOnce = INIT_ONCE_STATIC_INIT;

static OfferIn *parse_offer(int mainConn, const BYTE *p, int n)
{
    OfferIn *o;
    UINT32   cnt, i;
    const BYTE *q = p + 12, *end = p + n;
    size_t   chars = 0;

    if (n < 12) return NULL;
    cnt = le32p(p + 8);
    if (!cnt || cnt > FC_MAX_ENTRIES) return NULL;
    o = (OfferIn *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(OfferIn));
    if (!o) return NULL;
    o->ref = 1;
    o->id  = le64(p);
    o->mainConn = mainConn;
    o->e    = (FEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cnt * sizeof(FEntry));
    o->pool = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)n);      /* 名前の合計は n より短い */
    if (!o->e || !o->pool) goto bad;
    for (i = 0; i < cnt; i++) {
        FEntry *e = &o->e[i];
        UINT16  nl;
        if (q + 23 > end) goto bad;
        e->dir   = (q[0] & FE_DIR) != 0;
        e->attr  = le32p(q + 1);
        e->size  = le64(q + 5);
        e->mtime = le64(q + 13);
        nl = (UINT16)(q[21] | (q[22] << 8));
        q += 23;
        if (!nl || nl >= MAX_PATH || q + nl * 2 > end) goto bad;
        e->name = o->pool + chars;
        CopyMemory(e->name, q, nl * 2);
        e->name[nl] = 0;
        chars += nl + 1;
        q += nl * 2;
    }
    o->n = (int)cnt;
    InitializeCriticalSection(&o->pf);
    return o;
bad:
    if (o->e) HeapFree(GetProcessHeap(), 0, o->e);
    if (o->pool) HeapFree(GetProcessHeap(), 0, o->pool);
    HeapFree(GetProcessHeap(), 0, o);
    return NULL;
}

static IDataObject *g_current;      /* いまクリップボードに置いているもの(このスレッドだけが触る) */

static DWORD WINAPI clip_thread(LPVOID arg)
{
    MSG msg;
    (void)arg;
    OleInitialize(NULL);
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(g_clipReady);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd) { DispatchMessageW(&msg); continue; }
        if (msg.message == FM_OFFER) {
            ClipData *cd = (ClipData *)msg.lParam;
            OfferIn  *o  = parse_offer((int)msg.wParam, cd->data, cd->len);
            HeapFree(GetProcessHeap(), 0, cd);
            if (o) {
                VData *d = (VData *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VData));
                if (d) {
                    HRESULT hr;
                    d->iface.lpVtbl = &g_vdVtbl;
                    d->ref = 1;
                    d->o   = o;
                    hr = OleSetClipboard(&d->iface);
                    if (SUCCEEDED(hr)) {
                        if (g_current) IDataObject_Release(g_current);
                        g_current = &d->iface;              /* 1 つはこちらで持っておく */
                        log_printf(L"ファイル %d 件をクリップボードに置きました(貼り付けると中身を取りに行く)", o->n);
                        if (g_trayWnd)
                            PostMessageW(g_trayWnd, WM_APP_CLIPMARK, msg.wParam, (LPARAM)GetClipboardSequenceNumber());
                    } else {
                        log_printf(L"ファイルの一覧をクリップボードに置けませんでした (0x%08lX)", hr);
                        IDataObject_Release(&d->iface);
                    }
                } else offer_in_release(o);
            } else {
                log_printf(L"受け取ったファイルの一覧を読めませんでした");
            }
        } else if (msg.message == FM_QUIT) {
            break;
        }
    }
    if (g_current) {
        if (OleIsCurrentClipboard(g_current) == S_OK) OleSetClipboard(NULL);
        IDataObject_Release(g_current);
        g_current = NULL;
    }
    OleUninitialize();
    return 0;
}

static BOOL CALLBACK clip_init(PINIT_ONCE io, PVOID p, PVOID *c)
{
    (void)io; (void)p; (void)c;
    g_cfDesc     = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    g_cfContents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    g_cfEffect   = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    g_clipReady  = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_clipThread = CreateThread(NULL, 0, clip_thread, NULL, 0, &g_clipTid);
    if (!g_clipThread) return FALSE;
    WaitForSingleObject(g_clipReady, 5000);
    return TRUE;
}

/* net スレッドから: 相手がファイルをコピーしてこちらへ移ってきた */
void filecopy_offer_received(int mainConn, const BYTE *p, int n)
{
    ClipData *cd;
    if (g_dryRun) { log_printf(L"[dryrun] ファイルの一覧を受けました(%d バイト)", n); return; }
    if (!InitOnceExecuteOnce(&g_clipOnce, clip_init, NULL, NULL)) return;
    cd = (ClipData *)HeapAlloc(GetProcessHeap(), 0, sizeof(ClipData) + n);
    if (!cd) return;
    cd->len = n;
    CopyMemory(cd->data, p, n);
    if (!PostThreadMessageW(g_clipTid, FM_OFFER, (WPARAM)mainConn, (LPARAM)cd)) HeapFree(GetProcessHeap(), 0, cd);
}

void filecopy_stop(void)
{
    if (g_clipThread) {
        PostThreadMessageW(g_clipTid, FM_QUIT, 0, 0);
        WaitForSingleObject(g_clipThread, 3000);
    }
    if (g_srvThread) {
        InterlockedExchange(&g_srvStop, 1);
        SetEvent(g_reqEvent);
        WaitForSingleObject(g_srvThread, 3000);
    }
}
