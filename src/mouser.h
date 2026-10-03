/* ==================================================================
 * mouser.h - 共通定義
 *
 *  input-mouser は 1 台のキーボード・マウスで、LAN でつながった
 *  ほかの PC を操作する(Input Director と同じ役目)。
 *
 *  どの PC でも同じ exe を動かす。
 *    ・操作する側(マスター)   設定に「相手の PC」を登録した PC。
 *                               低レベルフックで入力を拾い、相手へ送る。
 *    ・操作される側(スレーブ) 「ほかの PC からの操作を受け付ける」PC。
 *                               受け取った入力を SendInput で再現する。
 *  1 台が両方を兼ねてもよい。
 *
 *  スレッド
 *    UI     トレイ、設定画面、クリップボード
 *    hook   低レベルフックと切り替えの判断(優先度高。止まらない処理だけ)
 *    net    すべての通信(1 本のイベント ループ)。受けた入力の再現もここ
 *    接続   相手ごとに 1 本。名前解決と connect だけを受け持つ(待ちが長いので)
 * ================================================================== */
#ifndef MOUSER_H
#define MOUSER_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef  WINVER
#define WINVER       0x0A00

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#define APP_NAME     L"input-mouser"
#define APP_VERSION  L"v4"      /* リリースのタグ(vN)と同じ。表示はこのまま */

#define DEFAULT_PORT 31860
#define PEER_MAX     8          /* 登録できる相手の数 */
#define HOST_MAX     64
#define INCOMING_MAX 8          /* 同時に受け付けるマスターの数 */

/* SendInput に付ける目印。自分が再現した入力を自分のフックで拾わないため */
#define INJECT_MAGIC ((ULONG_PTR)0x494D5352u)   /* 'IMSR' */

/* 画面の辺 */
enum { SIDE_LEFT, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM, SIDE_NONE = 0xFF };

/* ホットキー。vk = 0 は「なし」 */
typedef struct {
    BYTE vk;
    BYTE mods;      /* HK_CTRL など */
} Hotkey;
#define HK_CTRL  0x01
#define HK_ALT   0x02
#define HK_SHIFT 0x04
#define HK_WIN   0x08

typedef struct {
    WCHAR  host[HOST_MAX];  /* PC 名か IP アドレス */
    int    port;
    int    gx, gy;          /* 配置(このPC が 0,0。右が +x、下が +y) */
    Hotkey hk;              /* この PC へ直接切り替える */
} Peer;

typedef struct {
    Peer   peers[PEER_MAX];
    int    npeers;
    BOOL   accept;          /* ほかの PC からの操作を受け付ける */
    int    port;            /* 受け付けるポート(TCP と探索の UDP) */
    BYTE   key[32];         /* パスワードから作った鍵(パスワードそのものは持たない) */
    BOOL   haveKey;         /* FALSE なら空のパスワードの鍵を使う */
    int    edgeDelay;       /* 端で押し続ける時間(ミリ秒) */
    int    corner;          /* 角の無効範囲(ピクセル) */
    BOOL   noDragSwitch;    /* ボタンを押している間は切り替えない */
    BOOL   clipboard;       /* クリップボードを共有する */
    int    clipMaxMB;       /* 送るクリップボードの上限(ini のみ) */
    Hotkey hkHome;          /* このPC に戻る */
    Hotkey hkLock;          /* 切り替えを固定する(入/切) */
    BOOL   admin;           /* 管理者として動かす(管理者のウィンドウも操作できるように) */
    BOOL   osd;             /* 切り替えたとき画面に小さく知らせる(ini のみ) */
    BOOL   log;
    int    theme;           /* 0 = システム / 1 = ライト / 2 = ダーク(ini のみ) */
} Config;

/* ------------------------------------------------------------------ */
/*  main.c                                                             */
/* ------------------------------------------------------------------ */

extern HINSTANCE g_inst;
extern WCHAR     g_exePath[MAX_PATH];
extern WCHAR     g_iniPath[MAX_PATH];
extern BOOL      g_customIni;
extern HWND      g_trayWnd;
extern WCHAR     g_hostName[HOST_MAX];  /* この PC の名前 */
extern WCHAR     g_bindAddr[64];        /* -bind(検証用。受け付けるアドレスを絞る) */
extern BOOL      g_dryRun;              /* -dryrun(検証用。入力を再現せずログに書く) */
extern BOOL      g_elevated;            /* 管理者として動いている */

/* UI スレッド(トレイのウィンドウ)への知らせ */
#define WM_APP_TRAY      (WM_APP + 1)
#define WM_APP_COMMAND   (WM_APP + 2)   /* 別インスタンスからの指示 */
#define WM_APP_STATUS    (WM_APP + 3)   /* 接続の状態が変わった */
#define WM_APP_SWITCHED  (WM_APP + 4)   /* wp = 新しい相手(-1 = このPC) lp = 前の相手 */
#define WM_APP_CLIPRECV  (WM_APP + 5)   /* wp = 接続 ID, lp = ClipData* */
#define WM_APP_CLIPSEND  (WM_APP + 6)   /* wp = 接続 ID。変わっていれば送る */
#define WM_APP_FOUND     (WM_APP + 7)   /* 探索の応答(探索を頼んだ窓へ) lp = Found* */

#define CMD_SETTINGS     1
#define CMD_EXIT         2
#define CMD_SWITCH       3              /* lp = 0 はこのPC、1.. は相手の番号 */

void log_printf(const WCHAR *fmt, ...);
void tray_update(void);
BOOL app_relaunch_elevated(const WCHAR *extra);

/* ------------------------------------------------------------------ */
/*  config.c                                                           */
/* ------------------------------------------------------------------ */

extern Config g_cfg;        /* 書き換えるのは UI スレッドだけ */

void config_load(void);
BOOL config_save(void);
void config_changed(void);  /* 保存し、フックと通信に知らせる */
void peer_defaults(Peer *p);
BOOL hotkey_parse(const WCHAR *s, Hotkey *hk);
void hotkey_format(const Hotkey *hk, WCHAR *buf, int cch);
BOOL config_set_password(const WCHAR *pw);
int  peer_at(const Config *c, int gx, int gy);   /* その位置の相手の番号。無ければ -1 */
void peer_free_cell(const Config *c, int *gx, int *gy);

/* ------------------------------------------------------------------ */
/*  crypto.c(Windows の CNG。外部のライブラリは使わない)               */
/* ------------------------------------------------------------------ */

typedef struct {
    void   *key;            /* BCRYPT_KEY_HANDLE */
    BYTE    dir;            /* 送る向き('M' か 'S')。nonce に入れる */
    UINT64  ctr;
} Gcm;

BOOL crypto_init(void);
void crypto_random(void *buf, ULONG len);
BOOL crypto_derive(const WCHAR *password, BYTE out[32]);
void crypto_hmac(const BYTE key[32], const void *a, int alen, const void *b, int blen,
                 const void *c, int clen, BYTE out[32]);
BOOL gcm_init(Gcm *g, const BYTE key[32], BYTE dir);
void gcm_free(Gcm *g);
BOOL gcm_seal(Gcm *g, const BYTE *in, ULONG len, BYTE *out);           /* out は len+16 */
BOOL gcm_open(Gcm *g, BYTE dir, const BYTE *in, ULONG len, BYTE *out);  /* len は tag 込み */

/* ------------------------------------------------------------------ */
/*  net.c                                                              */
/* ------------------------------------------------------------------ */

/* 通信の中身(平文の 1 バイト目) */
enum {
    M_ENTER = 1,    /* M→S  u8 入る辺(SIDE_NONE なら中央) u16 位置(0..65535) */
    M_LEAVE,        /* M→S  操作が離れた */
    M_MOVE,         /* M→S  i32 dx, i32 dy(物理ピクセル。加速は適用済み) */
    M_BUTTON,       /* M→S  u8 ボタン(0 左 1 右 2 中 3 X1 4 X2) u8 押した */
    M_WHEEL,        /* M→S  u8 横 i16 量 */
    M_KEY,          /* M→S  u16 vk u16 scan u8 flags(IKF_*) */
    M_CLIP,         /* 双方  クリップボード(clip.c の形式) */
    M_PING,         /* M→S */
    M_PONG,         /* S→M */
    M_EDGE,         /* S→M  u8 越えた辺(SIDE_NONE なら端を離れた) u16 位置 u16 角までの距離 */
    M_RELEASE       /* M→S  押したままのキー・ボタンをすべて離す */
};
#define IKF_UP       0x01
#define IKF_EXT      0x02
#define IKF_UNICODE  0x04    /* VK_PACKET。scan に文字 */

/* 相手の状態(設定画面とトレイに出す) */
enum { PS_OFF, PS_CONNECTING, PS_READY, PS_UNREACHABLE, PS_BADPASS, PS_REFUSED };

/* 接続 ID: 0..PEER_MAX-1 は登録した相手(こちらがマスター)。
   CONN_IN_BASE 以上はこちらへ来たマスター。 */
#define CONN_IN_BASE 1000

typedef struct {
    WCHAR name[HOST_MAX];
    WCHAR addr[64];
    int   port;
} Found;

extern volatile LONG g_peerStatus[PEER_MAX];

BOOL net_start(void);
void net_stop(void);
void net_config_changed(void);
void net_send(int conn, BYTE type, const void *data, int len);
void net_send_owned(int conn, BYTE type, void *heapData, int len);   /* data の解放は net が行う */
void net_discover(HWND notify);
int  net_incoming_names(WCHAR *buf, int cch);  /* 今つながっているマスターの名前 */
void net_reconnect_now(void);

/* ------------------------------------------------------------------ */
/*  hook.c(マスター側)                                                */
/* ------------------------------------------------------------------ */

extern volatile LONG g_target;      /* 今操作している相手。-1 = このPC */
extern volatile LONG g_locked;      /* 切り替えを固定中 */
extern volatile LONG g_hotkeyCapture;   /* ホットキーの欄に入力中(ホットキーを効かせない) */

void hook_start(void);
void hook_stop(void);
void hook_config_changed(void);
void hook_switch(int peer);         /* -1 = このPC に戻る */
void hook_set_paused(BOOL on);
BOOL hook_paused(void);
void hook_toggle_lock(void);
/* net スレッドから */
void hook_peer_edge(int peer, int side, int pos, int cornerDist);
void hook_peer_down(int peer);

/* ------------------------------------------------------------------ */
/*  inject.c(スレーブ側。net スレッドで動く)                          */
/* ------------------------------------------------------------------ */

void inj_enter(int conn, int side, int pos);
void inj_leave(int conn);
void inj_move(int conn, int dx, int dy);
void inj_button(int btn, BOOL down);
void inj_wheel(BOOL horizontal, int delta);
void inj_key(int vk, int scan, int flags);
void inj_release_all(void);
void inj_conn_closed(int conn);
int  inj_active_conn(void);

/* 画面の上で cur から target へ動かす。行き先を out に入れ、
   画面の外へ押し出したときは越えた辺を返す(なければ SIDE_NONE)。
   pos は辺に沿った位置(0..65535)、cornerDist は角までの近いほうの距離。 */
int  screen_step(POINT cur, POINT target, POINT *out, int *pos, int *cornerDist);
/* 辺 side の pos の位置に当たる画面上の点(入ってくるときに使う) */
POINT screen_entry(int side, int pos);
void screen_virtual(RECT *r);

/* ------------------------------------------------------------------ */
/*  clip.c(UI スレッド)                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int  len;
    BYTE data[1];
} ClipData;

void clip_send_if_changed(int conn);
void clip_received(int conn, ClipData *cd);
void clip_conn_closed(int conn);

/* ------------------------------------------------------------------ */
/*  theme.c                                                            */
/* ------------------------------------------------------------------ */

void     theme_init(void);
BOOL     theme_refresh(void);
BOOL     theme_is_dark(void);
COLORREF theme_back(void);
COLORREF theme_footer(void);
COLORREF theme_ctrl_back(void);
COLORREF theme_text(void);
COLORREF theme_dim_text(void);
COLORREF theme_line(void);
HBRUSH   theme_back_brush(void);
HBRUSH   theme_footer_brush(void);
HBRUSH   theme_ctrl_brush(void);
void     theme_allow_dark(HWND hwnd);
void     theme_apply_dialog(HWND dlg);
LRESULT  theme_ctlcolor(UINT msg, HDC dc, HWND ctl, BOOL dimText);
BOOL     theme_custom_draw_button(NMCUSTOMDRAW *cd, LRESULT *result);

/* ------------------------------------------------------------------ */
/*  ui_*.c                                                             */
/* ------------------------------------------------------------------ */

void ui_open_main(void);
BOOL ui_dialog_message(MSG *msg);
void ui_theme_changed(void);
void ui_status_changed(void);
BOOL ui_edit_peer(HWND owner, Peer *p, BOOL isNew);
BOOL ui_password(HWND owner);

typedef struct {
    HFONT heading;
    int   footerTop;
} DlgLook;

void dlg_look_init(HWND dlg, DlgLook *lk, const int *headingIds, int nHeadings, int footerAnchorId);
void dlg_look_free(DlgLook *lk);
BOOL dlg_look_erase(HWND dlg, HDC dc, const DlgLook *lk);
int  ui_message(HWND owner, const WCHAR *main, const WCHAR *content, TASKDIALOG_COMMON_BUTTON_FLAGS buttons, PCWSTR icon);
void ui_set_icons(HWND h);
void hotkey_edit_attach(HWND edit, const Hotkey *hk);
void hotkey_edit_get(HWND edit, Hotkey *hk);
const WCHAR *peer_status_text(int st);
void osd_show(const WCHAR *text);

/* 配置図(ui_layout.c) */
#define LAYOUT_CLASS  L"InputMouser.Layout"
#define LN_SELCHANGE  1     /* WM_COMMAND の通知コード */
#define LN_ACTIVATE   2     /* ダブルクリック / Enter */
#define LN_MOVED      3     /* ドラッグで並びが変わった(g_cfg は書き換え済み) */
#define LN_DELETE     4     /* Delete キー */
void layout_register(void);
int  layout_selected(HWND h);           /* 選んでいる相手の番号。なければ -1 */
void layout_select(HWND h, int peer);

#define WM_APP_RELOOK    (WM_APP + 20)

#endif
