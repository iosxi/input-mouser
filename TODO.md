# input-mouser の TODO

まだ手を付けていない報告や気づき。片付けたら消し、直した版をコミットに書く。

## iiv で見ている PC へ移ったとき、iiv-client の中にカーソルが出ない(2026-10-07、利用者の報告)

変則的な使い方による指摘なので、記録だけしておく(利用者の指示)。

### 使い方

- MSI: input-mouser と iiv-server が動いている。マウスはつないでいない(input-mouser で操作する)。
- DESKTOP-6CENMT2: input-mouser と iiv-client。iiv-client で MSI の画面を見ている。
- DESKTOP-6CENMT2 のマウスを画面の端の外へ動かし、input-mouser で MSI を操作する。
  そのとき MSI の画面を iiv-client の窓で見る。

### 起きたこと

| 「マウスのない PC でもカーソルを表示する」 | iiv-client の中の MSI のカーソル |
| --- | --- |
| ON | 表示されない(MSI 側の操作そのものは正常) |
| OFF | わずかに表示される。カーソルの動きが止まると表示されなくなる |

iiv-client の窓の中にマウスを置いて(iiv で)操作するときは、何の問題もない。

### 見当(どれも確かめていない)

- ON のときのカーソルの見せ方(`src/cursor.c`。CLAUDE.md の v6「マウスのない PC のカーソル」・v8「マウスキー方式」・
  v9「Num Lock に合わせる」)が、Windows のカーソルとして「見えている」状態になっていない、という筋。
  iiv-server はカーソルを画面の絵とは別に、形と位置と「見える印」で送る(iiv の `IIV_S_CURSOR_SHAPE` /
  `IIV_S_CURSOR_POS`)ので、Windows が「見えない」と答えるカーソルは iiv-client に出ない。
- OFF のときは、マウスのない PC では Windows がカーソルを隠していて、注入した動きの間だけ一瞬見える、という筋。
- 確かめるなら: MSI で ON・OFF それぞれ、`GetCursorInfo` の flags(`CURSOR_SHOWING` / `CURSOR_SUPPRESSED`)と、
  iiv-server のログ(カーソルの見える印)を見る。iiv 側で直すべきか、input-mouser 側で直すべきかはそれから決める。
