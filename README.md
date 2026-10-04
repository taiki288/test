# hexa-udon v2 production package

16×16、24×24、32×32のv2 profileを使う本番clientのsource-only treeです。主PC単独のstrict-verified baselineを標準とし、LAN workerは明示opt-inの候補計算補助です。主PCだけが公式token、base URL、HTTP、POSTを扱い、workerの失敗時は主PCへ戻ります。

明示allowlistのCMake/source/header、v2 profile三種、当日運用文書、`PACKAGE-MANIFEST.json`だけを含みます。v1、実験、Catalog、研究snapshot、Dashboard、Harness、Replay、tests、rehearsal driver、scripts、results、Git metadataは含みません。

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

## 秘密情報

公式tokenは環境変数からだけ渡します。worker secretは公式tokenと別の環境変数にし、どちらもsource、Session、log、manifest、標準出力へ保存しません。実会場base URLは運営から受け取り、コードや文書へ固定しません。

## 主PC単独の起動

profile未指定の既定動作は変えず、対応sizeを最初の認証済み`GET /setting`から選ぶ場合だけ`--profile-set v2`を明示します。settingは一度だけ取得し、Day0 type selectorへ再利用します。

```bash
export PROCON_TOKEN='安全な環境で設定した値'
../30013-prod-build/hexa_udon auto --base-url "$VENUE_BASE_URL" \
  --token-env PROCON_TOKEN --profile-set v2 \
  --session-dir /secure/runtime/match-session \
  --log-dir /secure/runtime/match-log
```

上記はdry-runです。当日、提出を明示承認した場合だけ同じコマンドへ`--execute`を追加します。当日以外は`--execute`を実行しないでください。

## LAN worker（任意）

workerはloopbackまたは明示したprivate LAN addressで起動し、公式token/base URLを渡しません。

```bash
export HEXA_LAN_WORKER_SECRET='公式tokenとは別の値'
./hexa_udon worker --listen 192.168.1.20:40123 --worker-token-env HEXA_LAN_WORKER_SECRET
./hexa_udon auto --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement \
  --lan-worker 192.168.1.20:40123 --lan-worker-timeout-ms 250
```

workerには現在日のcanonical PlannerInputだけを渡します。主PCは先にbaselineを検証し、応答を独立Simulatorで再検証します。timeout、切断、identity不一致、低score、Simulator失敗ではworkerを待たずbaseline-retainedへ戻ります。loopbackで確認済みですが、実LAN実APIは第49段階で確認予定です。詳細は[LAN運用](docs/lan-worker.md)を参照してください。

## 当日運用

 [運用手順](docs/operations.md)、[停止・Recovery](docs/recovery.md)、[認証と秘密管理](docs/safety.md)を確認してください。主PC一台・main client一プロセスだけで運用します。v2を対応サイズで明示指定するか、練習場の一発起動では`--profile-set v2`を使って最初の`GET /setting`からsize/agent数に対応するv2を一意に選びます（settingの追加取得はありません）。Day0 `/setting`だけでtypeを選び、毎日の現在観測値からbaseline-firstで計画します。日次は実`endsAt`、strict Simulator、reserve、余剰時だけの改善探索を使います。

`--execute`は競技提出（POST）を有効にする操作です。package生成・build/help/schema検証では実行しません。

## ユーザー自身が別repo化・更新する場合

開発repoの`package-production`で新しい空directoryへ生成し、clean build/help/schemaとmanifestを確認してください。既存submission repoへ直接生成しないでください。

新しい別repoを作る場合に限り、ユーザー自身がpackage directoryで`git init`、内容確認、commit/pushを行います。既存submission repoを更新する場合は、まず未コミット変更とSession/log/tokenの保存先を確認・退避し、新packageとのdiffをレビューしてから必要なsourceを反映してください。既存treeを丸ごと削除・上書きせず、v1や古い開発ファイルの除去はユーザーが明示的にレビューしてください。runtime Sessionをsource更新に混ぜないでください。generatorはGitやGitHubを操作しません。

manifestの`sourceCommit`は読取可能な生成元HEAD識別子です。未コミット変更を含む実際の配布内容は各file SHA-256で識別します。時刻・絶対home path・remote URL・環境変数値はmanifestに保存しません。
