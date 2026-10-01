# hexa-udon production package

このtreeは、本番クライアントとそのビルド・運用に必要なallowlistだけで構成されます。実験、Dashboard、Harness、tests、resultsは含みません。

## ビルド

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/hexa_udon --help
```

`nlohmann_json`と`libcurl`は実行環境の依存として事前に用意してください。

## 運用

認証tokenは`PROCON_TOKEN`などの環境変数で渡し、ファイル・Session・ログへ保存しません。16×16、24×24、32×32のprofileは`auto --profile config/profiles/<file>.json`で明示指定します。profile未指定の既定動作は変わりません。

PreMatch type selectorは試合前のDay0 `/setting`（サイズ、agent位置、燃料、全道路順調）だけを使います。Day1以降は当日の観測状態からGreedy-refuelを再計画し、strict Simulatorで検証できた計画だけを提出候補にします。未来snapshotや研究成果物は本番入力に使いません。

本番は主PC単独で完結します。補助PC LAN機能は未実装であり、将来拡張です。

`--execute`は競技提出操作です。このpackage生成・検証では実行しません。

## 別repoにする場合

ユーザーがpackage生成後にこのdirectoryで`git init`し、内容を確認してから自分でcommit・pushしてください。packaging scriptはGit初期化、commit、GitHub repo作成、pushを呼びません。
