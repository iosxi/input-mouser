/* ==================================================================
 * clip.c - クリップボードの共有(UI スレッド)
 *
 *  Input Director と同じく、操作が移るときだけ受け渡す。
 *      マスター → 相手   相手へ移ったとき
 *      相手 → マスター   相手から離れたとき(M_LEAVE を受けたスレーブが送る)
 *  前回渡したときから変わっていなければ送らない(接続ごとに
 *  GetClipboardSequenceNumber を覚えておく)。
 *
 *  形式  [個数 u8] { [種類 u8] [長さ u32] [中身] } ...
 *        種類 1 = CF_UNICODETEXT、2 = CF_DIB
 *  ファイル(CF_HDROP)は送らない。
 * ================================================================== */

#include "mouser.h"

enum { CK_TEXT = 1, CK_DIB = 2 };

static struct { int conn; DWORD seq; BOOL used; } g_sent[PEER_MAX + INCOMING_MAX * 2];

static DWORD *sent_slot(int conn, BOOL create)
{
    int i, free_ = -1;
    for (i = 0; i < (int)ARRAYSIZE(g_sent); i++) {
        if (g_sent[i].used && g_sent[i].conn == conn) return &g_sent[i].seq;
        if (!g_sent[i].used && free_ < 0) free_ = i;
    }
    if (!create) return NULL;
    if (free_ < 0) free_ = 0;               /* あふれたら古いものを使い回す */
    g_sent[free_].used = TRUE;
    g_sent[free_].conn = conn;
    g_sent[free_].seq  = 0;
    return &g_sent[free_].seq;
}

/* 相手から受け取った内容をクリップボードに置いた。その相手には送り返さない */
void clip_mark_synced(int conn, DWORD seq)
{
    DWORD *slot = sent_slot(conn, TRUE);
    *slot = seq;
}

/* 検証用: 変わっていなくても送る */
void clip_force_send(int conn)
{
    DWORD *slot = sent_slot(conn, TRUE);
    *slot = 0;
    clip_send_if_changed(conn);
}

void clip_conn_closed(int conn)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(g_sent); i++)
        if (g_sent[i].used && g_sent[i].conn == conn) g_sent[i].used = FALSE;
}

static BOOL open_clipboard(void)
{
    int i;
    for (i = 0; i < 10; i++) {
        if (OpenClipboard(g_trayWnd)) return TRUE;
        Sleep(15);                          /* ほかのアプリが開いている */
    }
    return FALSE;
}

static void put32(BYTE *p, UINT32 v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }
static UINT32 le32(const BYTE *p) { return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }

void clip_send_if_changed(int conn)
{
    DWORD  seq = GetClipboardSequenceNumber(), *slot;
    HANDLE ht = NULL, hd = NULL;
    SIZE_T tl = 0, dl = 0, total;
    BYTE  *buf, *p;
    int    count = 0;
    SIZE_T limit = (SIZE_T)g_cfg.clipMaxMB * 1024 * 1024;

    if (!g_cfg.clipboard || g_dryRun) return;     /* 検証用の受け手は送らない */
    slot = sent_slot(conn, TRUE);
    if (*slot == seq) return;
    *slot = seq;
    if (!open_clipboard()) return;

    /* エクスプローラーでコピーしたファイル。一覧だけ送り、中身は貼り付けたときに(filecopy.c) */
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
        BYTE *msg = NULL;
        int   mlen = 0;
        BOOL  ok = hd && filecopy_make_offer(conn, hd, &msg, &mlen);
        CloseClipboard();
        if (ok) {
            net_send_owned(conn, M_FILES, msg, mlen);
            if (conn < CONN_IN_BASE) net_file_open(conn);     /* こちらが操作する側。先に張っておく */
        }
        return;
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT) && (ht = GetClipboardData(CF_UNICODETEXT)) != NULL) {
        const WCHAR *t = (const WCHAR *)GlobalLock(ht);
        if (t) {
            SIZE_T max = GlobalSize(ht) / sizeof(WCHAR), n = 0;
            while (n < max && t[n]) n++;
            tl = (n + 1) * sizeof(WCHAR);
            GlobalUnlock(ht);
        } else ht = NULL;
    }
    if (IsClipboardFormatAvailable(CF_DIB) && (hd = GetClipboardData(CF_DIB)) != NULL)
        dl = GlobalSize(hd);

    if (tl > limit) tl = 0;
    if (dl > limit || tl + dl > limit) {
        if (dl) log_printf(L"クリップボードの画像が大きすぎるので送りません(%lu KB)", (ULONG)(dl / 1024));
        dl = 0;
    }
    total = 1 + (tl ? 5 + tl : 0) + (dl ? 5 + dl : 0);
    if (total == 1) { CloseClipboard(); return; }

    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, total);
    if (!buf) { CloseClipboard(); return; }
    p = buf + 1;
    if (tl) {
        const BYTE *t = (const BYTE *)GlobalLock(ht);
        if (t) {
            *p++ = CK_TEXT; put32(p, (UINT32)tl); p += 4;
            CopyMemory(p, t, tl - 2);
            p[tl - 2] = p[tl - 1] = 0;
            p += tl;
            count++;
            GlobalUnlock(ht);
        }
    }
    if (dl) {
        const BYTE *d = (const BYTE *)GlobalLock(hd);
        if (d) {
            *p++ = CK_DIB; put32(p, (UINT32)dl); p += 4;
            CopyMemory(p, d, dl);
            p += dl;
            count++;
            GlobalUnlock(hd);
        }
    }
    CloseClipboard();
    buf[0] = (BYTE)count;
    if (!count) { HeapFree(GetProcessHeap(), 0, buf); return; }
    log_printf(L"クリップボードを送りました(文字 %lu 字、画像 %lu KB)",
               (ULONG)(tl ? tl / 2 - 1 : 0), (ULONG)(dl / 1024));
    net_send_owned(conn, M_CLIP, buf, (int)(p - buf));
}

void clip_received(int conn, ClipData *cd)
{
    const BYTE *p = cd->data, *end = cd->data + cd->len;
    int   n, i, done = 0;

    if (!g_cfg.clipboard || cd->len < 1) goto out;
    if (g_dryRun) {
        log_printf(L"[dryrun] クリップボードを受けました(%d バイト)", cd->len);
        goto out;
    }
    n = *p++;
    if (!open_clipboard()) goto out;
    EmptyClipboard();
    for (i = 0; i < n && p + 5 <= end; i++) {
        BYTE   kind = p[0];
        UINT32 len  = le32(p + 1);
        HGLOBAL g;
        p += 5;
        if (len > (UINT32)(end - p)) break;
        g = GlobalAlloc(GMEM_MOVEABLE, len ? len : 1);
        if (g) {
            BYTE *d = (BYTE *)GlobalLock(g);
            if (d) { CopyMemory(d, p, len); GlobalUnlock(g); }
            if (SetClipboardData(kind == CK_TEXT ? CF_UNICODETEXT : CF_DIB, g)) done++;
            else GlobalFree(g);
        }
        p += len;
    }
    CloseClipboard();
    {
        DWORD *slot = sent_slot(conn, TRUE);
        *slot = GetClipboardSequenceNumber();   /* 送り主はもう持っている */
    }
    if (done) log_printf(L"クリップボードを受け取りました");

    /* マスターが別の相手を操作中なら、そちらへも回す(相手から相手へ移ったとき) */
    if (conn < CONN_IN_BASE && g_target >= 0 && g_target != conn) clip_send_if_changed((int)g_target);
out:
    HeapFree(GetProcessHeap(), 0, cd);
}
