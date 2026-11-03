# System 2の待機アニメーション

ユーザー提供の「思考中の吹き出しスプライトシート.png」を内蔵image_genで透過化し、`scripts/split-thinking-sprites.ps1`で枠を除外して8コマに分割した。分割時には吹き出しと2つの小円以外の細かな残留ノイズも除去する。元のDownloads内の画像は変更しない。

- `cutout-source.png`: image_genによる編集結果。再分割用の素材であり、アプリには同梱しない。
- `frame-00.png`〜`frame-07.png`: 各272×272の透過PNG。
- `sprite-sheet.png`: 実行時に使用する2176×272の8コマアトラス。
- 点の順番: 0 → 1 → 2 → 3 → 3 → 2 → 1 → 0。毎秒5コマ、1周1.6秒。

`ConversationManager.system2Waiting`がtrueの間、`AvatarView.qml`のAnimatedSpriteがキャラクターの頭の右横でループする。THINKで通信を開始してから最初の回答テキストを受信するまでが表示対象。LOCAL、CLARIFY、回答受信後、キャンセル、エラーでは非表示となる。次の待機では最初のコマから再開する。

再分割はリポジトリのルートからPowerShell 7で次のように実行する。

```powershell
./scripts/split-thinking-sprites.ps1
```

使用した内蔵image_genの編集プロンプト:

> Edit target: the supplied eight-frame thought bubble sprite sheet. Use case: background-extraction. Remove ALL rectangular grid frames, horizontal and vertical dividing lines, black background, stray white artifacts and colored noise around the bubbles. Preserve the original rounded white/cloud thought bubble with its gray dimensional outline and its two small trailing circles at lower left. Preserve all EIGHT animation states in reading order with internal dot counts exactly 0,1,2,3,3,2,1,0. Keep every bubble geometrically identical, same scale and aligned to same baseline, with only the internal dots changing. Arrange as exactly eight equally sized square cells in a SINGLE horizontal row, one bubble centered in each cell, consistent generous transparent padding to prevent clipping. Eight columns and one row, no extra rows. True RGBA transparent background outside each bubble and trailing circles; NO checkerboard painted into image, no solid backdrop. No visible cell boundaries, no frames, no captions, no labels, no letters. This is an in-app waiting animation asset. Retain the supplied art style, do not redesign the bubble. Prefer wide sprite sheet with overall 8:1 aspect ratio.
