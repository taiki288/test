# hexa-udon v2 production package

本番clientのsource-only treeです。明示allowlistのCMake/source/header、v2 profile三種、当日運用文書、`PACKAGE-MANIFEST.json`だけを含みます。v1、実験、Catalog、研究snapshot、Dashboard、Harness、Replay、tests、rehearsal driver、scripts、results、Git metadataは含みません。profile内の承認根拠metadataは不変のまま保持し、実験成果物を読み込む用途には使いません。

## Clean Release build（通信なし）

C++20対応compiler、CMake、nlohmann_json 3.11.3以上、libcurlを事前に用意してください。CMakeは依存をネットワークから取得しません。

```bash
cmake -S . -B ../30013-prod-build -DCMAKE_BUILD_TYPE=Release
cmake --build ../30013-prod-build --parallel 4
../30013-prod-build/hexa_udon --help
../30013-prod-build/hexa_udon validate-profile --profile config/profiles/16x16-one-supply-v2.json
../30013-prod-build/hexa_udon validate-profile --profile config/profiles/24x24-two-supply-v2.json
../30013-prod-build/hexa_udon validate-profile --profile config/profiles/32x32-one-or-three-supply-v2.json
```

build directoryはpackage外の新規directoryに置くと、配布manifestのsource hash照合を維持できます。上記確認はtoken不要・HTTP/POSTなしです。package内では開発用loopback rehearsalを実行しません。

## 当日運用

[運用手順](docs/operations.md)、[停止・Recovery](docs/recovery.md)、[認証と秘密管理](docs/safety.md)を確認してください。主PC一台・main client一プロセスだけで運用します。v2を対応サイズで明示指定し、Day0 `/setting`だけでtypeを選び、毎日の現在観測値からbaseline-firstで計画します。日次は実`endsAt`、strict Simulator、reserve、余剰時だけの改善探索を使います。

`--execute`は競技提出（POST）を有効にする操作です。package生成・build/help/schema検証では実行しません。LAN補助PC機能は未実装で、当日導線に含めません。

## ユーザー自身が別repo化・更新する場合

開発repoの`package-production`で新しい空directoryへ生成し、clean build/help/schemaとmanifestを確認してください。既存submission repoへ直接生成しないでください。

新しい別repoを作る場合に限り、ユーザー自身がpackage directoryで`git init`、内容確認、commit/pushを行います。既存submission repoを更新する場合は、まず未コミット変更とSession/log/tokenの保存先を確認・退避し、新packageとのdiffをレビューしてから必要なsourceを反映してください。既存treeを丸ごと削除・上書きせず、v1や古い開発ファイルの除去はユーザーが明示的にレビューしてください。runtime Sessionをsource更新に混ぜないでください。generatorはGitやGitHubを操作しません。

manifestの`sourceCommit`は読取可能な生成元HEAD識別子です。未コミット変更を含む実際の配布内容は各file SHA-256で識別します。時刻・絶対home path・remote URL・環境変数値はmanifestに保存しません。
