# hexa-udon

## 1. 概要

hexa-udonは、16×16、24×24、32×32のv2 profileで日次計画を作成するC++20クライアントです。主PCが公式APIとの通信、baseline生成、strict Simulatorによる再検証、OfficialScoreによる採否、必要なPOSTを担当します。LAN workerは任意の候補計算補助であり、公式API、公式base URL、公式token、提出POSTを扱いません。

本番では、GitHubの`main`を共有repo兼提出対象とします。実行時データ、build成果物、Session/log、token、secretはsource treeと分離します。

## 2. 本番チートシート

以下は本番当日の実行順です。URL、token、secretは安全な環境から設定し、READMEやGitへ実値を書きません。`/secure/runtime`は実運用で用意した保護directoryの例です。

### 2.1 環境変数を設定する

```bash
export VENUE_BASE_URL='https://<venue-host>'
export PROCON_TOKEN='<official-token>'
export HEXA_LAN_WORKER_SECRET='<worker-only-secret>'
```

### 2.2 clean Release buildを作る

空の`build-release`を使用し、Debug/Releaseでbuild directoryを共用しません。

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

### 2.3 profile、CLI、CTestを確認する

```bash
./build-release/hexa_udon validate-profile --profile config/profiles/16x16-one-supply-v2.json
./build-release/hexa_udon validate-profile --profile config/profiles/24x24-two-supply-v2.json
./build-release/hexa_udon validate-profile --profile config/profiles/32x32-one-or-three-supply-v2.json
./build-release/hexa_udon --help

ctest --test-dir build-release -R \
  'app_unit_tests|lan_worker_unit_tests|daily_deadline_policy_tests|control_tests|protocol_tests|session_tests' \
  --output-on-failure
```

### 2.4 API接続を確認する

```bash
./build-release/hexa_udon check \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --log-dir /secure/runtime/log
```

### 2.5 workerを起動する（使用する場合だけ）

workerはloopbackまたはprivate LANのaddressで起動します。公式tokenではなく、worker専用secretだけを読みます。

```bash
./build-release/hexa_udon worker \
  --listen 127.0.0.1:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET
```

### 2.6 dry-runで日次経路を確認する

`auto`は`--execute`なしではdry-runです。dry-runでもGET観測は行いますが、競技POSTは行いません。

```bash
./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement \
  --lan-worker 127.0.0.1:39001 --lan-worker-timeout-ms 10000 \
  --session-dir /secure/runtime/session \
  --log-dir /secure/runtime/log
```

### 2.7 executeを明示して本番実行する

dry-runの結果、profile、types、Session、log、時刻を人手で確認してから`--execute`を付けます。

```bash
./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement --execute \
  --lan-worker 127.0.0.1:39001 --lan-worker-timeout-ms 10000 \
  --session-dir /secure/runtime/session \
  --log-dir /secure/runtime/log
```

### 2.8 終了後に状態を確認する

```bash
./build-release/hexa_udon show-state \
  --session-dir /secure/runtime/session
```

必要な場合だけ、まずdry-runの`recover`でSessionを確認します。公式状態を人手で照合し、再実行が必要と判断した場合に限り`--execute`を明示します。POST結果不明後の自動再送はしません。

```bash
./build-release/hexa_udon recover \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --session-dir /secure/runtime/session \
  --log-dir /secure/runtime/log
```

### 2.9 異常時の停止点

- workerのtimeout、transport failure、claim mismatch、strict failureは、そのworkerだけを失敗扱いにします。他workerとmainの処理は継続し、採用不能ならbaselineへfallbackします。
- 403/429は安全な分類、`Retry-After`、bounded backoff、deadlineをOperationLogで確認します。deadlineを越える再試行や新規GET/POSTは開始しません。
- POST前のtimeoutは`submissionAttempted=false`です。POST後に応答が不明なら`null`となり、`RecoveryRequired`で停止します。公式状態を確認するまで再送しません。
- `RecoveryRequired`、claim mismatch、strict failure、deadline不足は本番の停止・人手確認が必要な警告です。

### 2.10 Session/logを確認する

終了後は`show-state`と保護されたSession/log directoryを確認し、最終score、candidateSource、adoption結果、POST結果、RecoveryRequiredの有無を記録します。Session/logを提出物やGitへコピーしません。

## 3. 初期セットアップ

必要なものはC++20対応コンパイラ、CMake 3.20以上、nlohmann_json 3.11.3以上、libcurlです。CMakeは依存関係をネットワークから取得しません。`runtime/`、`results/`、build成果物、Session/log、token、secretはGit管理対象外です。

環境変数は次の役割に分かれます。

- `VENUE_BASE_URL`: 主PCが接続する公式または練習場のbase URL。
- `PROCON_TOKEN`: 主PCだけが読む公式token。
- `HEXA_LAN_WORKER_SECRET`: worker protocol専用secret。公式tokenとは別物です。

## 4. ビルド

構成ごとに専用directoryを使用します。Debug、Release、Sanitizerの設定確認はできますが、本番当日はcleanな`build-release`から実行します。

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel 4

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

## 5. CLIリファレンス

共通の接続系コマンドは`--base-url URL`、`--token-env NAME`、`--session-dir DIR`、`--log-dir DIR`、`--poll-ms N`、`--connect-timeout-ms N`、`--total-timeout-ms N`、`--safety-seconds N`、`--max-get-retries N`、`--log-level info|warning|error`を使用します。既定のSession/log directoryはそれぞれ`run/session`、`run/log`です。本番では保護された外部directoryを明示してください。

### コマンド

- `check --base-url URL`: API接続と安全な応答分類を確認します。
- `auto --base-url URL`: 日次計画を実行します。`--execute`がある場合だけPOSTします。
- `recover --base-url URL`: 保存Sessionから復旧確認を行います。`--execute`なしがdry-runです。
- `show-state --session-dir DIR`: Sessionの最後の状態を表示します。`--execute`は使用しません。
- `validate-profile --profile FILE`: 通信なしでprofileを検証します。
- `worker --listen HOST:PORT --worker-token-env NAME`: worker serverを起動します。

### 計画・profile・worker option

`auto`と`recover`では`--planner wait|greedy|greedy-refuel|optimized|daily-improvement`、`--planner-ms N`、`--planner-candidates N`、`--seed N`、`--refuel-ms N`、`--refuel-candidates N`、`--rendezvous-candidates N`、`--max-refuels N`、`--daily-deadline-policy NAME`を使用できます。Optimizerには`--optimizer-ms N`と`--optimizer-iterations N`があります。

`auto`では`--profile-set v2`または`--profile FILE`を選択します。`--profile-set v2`は最初の`GET /setting`からprofileを選択し、`--profile FILE`は明示profileを使用します。profile指定時はtuning overrideを併用せず、`--types`だけを許可します。`--types 0,0,0,1`は明示type指定の例です。

type selectorは`--type-selector fixed|prematch`、`--type-selector-ms N`、`--type-selector-max-supply 0|1|2`、`--type-selector-min-supply 0|1|2`です。

`auto`だけで`--lan-worker HOST:PORT`を繰り返し指定でき、`--lan-worker-timeout-ms N`でworkerの上限を指定します。LAN workerは`daily-improvement`経路の候補計算補助です。

`--log-level info`は通常診断、`warning`はwarning/error、`error`はerrorだけを出力します。stdoutの詳細量を増やすための隠れたログ optionはありません。

## 6. profile

| profile | 盤面 | agent | supply allowlist | baseline / improvement deadline |
| --- | ---: | ---: | --- | --- |
| `config/profiles/16x16-one-supply-v2.json` | 16×16 | 4 | `[1]` | 1000ms / 10000ms |
| `config/profiles/24x24-two-supply-v2.json` | 24×24 | 5 | `[2]` | 2000ms / 20000ms |
| `config/profiles/32x32-one-or-three-supply-v2.json` | 32×32 | 7 | `[1,3]` | 5000ms / 30000ms |

全profileは`daily-improvement`、共有hard deadline、POST前の安全余裕を持ちます。profileのallowlist、盤面、agent数、deadline、reserve、minimum improvementはJSONを正とし、実行前に`validate-profile`で確認します。

## 7. 日次アルゴリズム

採用フローは次の順序です。

```text
baseline生成
  → main Optimizer / worker並行探索
  → claim確認
  → strict Simulator再検証
  → OfficialScore
  → DailyReadiness
  → deterministic tie-break
  → 採用またはbaseline fallback
  → POST
```

- baseline-firstでGreedy/Refuel baselineを生成し、strict Simulatorで先に検証します。
- GreedyとOptimizerは、募集要項の「偶数行が右にずれる」座標系に対応する共通距離helperを使用します。既存のfuel判定、axial座標修正、strict Simulator、OfficialScore、DailyReadiness、deterministic tie-breakを同じ契約で維持します。
- fuel判定、axial座標距離、Greedy planner、Refuel planner、AddUncollectedBrand、OfficialScore、DailyReadiness、type selectorは、profileと現在のstateを入力に決定的に評価します。
- worker replyはrequestIdDigest、input/state/map/profile identity、seed、index/countなどのclaimを主PCが確認し、strict Simulatorで再検証してから比較します。
- OfficialScore、DailyReadiness、deterministic tie-breakの順で採否を決めます。不正、timeout、claim mismatch、deadline超過、strict failureは採用せず、baselineまたはmain候補へfallbackします。
- `hardPlanningDeadline`は日次終了時刻から安全余裕を引いた境界です。`send_at >= deadline`では新しいstrict検証、GET、POSTを開始しません。

## 8. LAN worker運用

workerは任意機能です。workerを起動しなくてもmainのbaseline-first経路は成立します。workerは公式API、公式base URL、公式tokenを扱わず、worker専用secretだけでloopbackまたはprivate LANのrequest/replyを受けます。公開addressや`0.0.0.0`をlisten endpointにしません。

worker通信timeoutは`steady_clock`の絶対deadline方式です。worker処理開始時に作ったdeadlineから、connect、write、readの各操作直前に残り時間を再計算し、その残り時間だけをtimeoutとして渡します。残り時間が0以下なら次の通信操作を開始しません。deadline後のI/Oは開始しません。

timeoutしたworker、transport failure、claim mismatch、strict failureは、そのworkerだけを失敗扱いにします。他workerとmainの処理は継続し、候補が採用できなければbaseline/mainへfallbackします。`hardPlanningDeadline`、reply grace、`workerElapsedMs`、`mainElapsedMs`は日次の安全契約と診断に使用します。

実LAN、worker firewall、実API、実tokenでの相互運用は未検証です。loopback fixtureの成功を本番LAN完走の根拠にしません。

## 9. ログとSession

### stdoutに表示されるもの

stdoutは人間向けの簡潔な表示に限定します。起動モード、type選択完了、日次開始・終了、最終score、`candidateSource`、adoption結果、POST成功・失敗、timeout・claim mismatch・strict failure・`RecoveryRequired`など対応が必要な警告、最終終了状態を表示します。

workerごとのrequestIdDigest、inputHash、seed、worker index/count、candidate count、score/readiness digest、strict再検証詳細、rejection reason、elapsed time詳細、planner途中経過、rendezvous詳細、候補列挙はstdoutに出しません。

### OperationLog/Sessionに保存される診断

OperationLogとSession診断には、必要な範囲でrequest/response分類、Retry-After、bounded backoff、deadline、workerのdigest/hash/seed/index/count/candidate情報、score/readiness、strict結果、claim確認、`candidateSource`、`adoptionReason`、`workerElapsedMs`、`mainElapsedMs`を保存します。Sessionは復旧契約に必要な状態・receiptを保持するため、Git外の保護directoryで管理し、提出物へ含めません。

token、worker secret、HTTP本文、秘匿URL、認証情報、action全配列はstdoutやOperationLogへ保存・出力しません。Session/logを共有するときも秘密情報を含めないことを人手で確認します。

### RecoveryRequiredと自動再送

`submissionAttempted`は、POST前の`false`、POST応答を受理した`true`、POST後に結果が不明な`null`の三値です。`null`は公式側の受理状態を推測できないため`RecoveryRequired`となります。`show-state`、Session、公式状態を人手で照合し、同一状態を確認した上でのみ、明示的な`recover --execute`を検討します。同一POSTの複数回自動送信は契約上行いません。

## 10. テスト

CTestには次の6件が登録されています。

- `app_unit_tests`
- `lan_worker_unit_tests`
- `daily_deadline_policy_tests`
- `control_tests`
- `protocol_tests`
- `session_tests`

```bash
ctest --test-dir build-release -R \
  'app_unit_tests|lan_worker_unit_tests|daily_deadline_policy_tests|control_tests|protocol_tests|session_tests' \
  --output-on-failure
```

開発時はDebug専用directoryで同じCTestを実行し、Sanitizerも専用directoryに分離します。距離fixtureは偶数行・奇数行の隣接、同一セル、複数行距離を、worker fixtureはconnect/write/readごとの残り時間短縮、deadline後I/O禁止、片worker timeout時の他worker継続、baseline fallbackを確認します。stdout fixtureは詳細診断のstdout漏出がなく、最終状態と対応警告が残ることを確認します。

## 11. 提出・運用ルール

GitHubの`main`は共有repo兼提出対象です。提出前は`src/`、`include/`、`config/profiles/`、CMake定義、必要なdocsからclean buildを作ります。提出対象にruntime、results、build成果物、Session/log、token、secretを含めません。

旧PACKAGE-MANIFEST、旧package生成手順、allowlist hash、source commit metadata、`~/30013-submit`は現行の実行・提出手順では使用しません。旧package工程は現行mainの提出経路の根拠にしません。

## 12. アルゴリズム実装場所

| 領域 | 主な実装 |
| --- | --- |
| API sequence、日次実行、候補採否 | `src/app/auto_client.cpp` |
| deadline、baseline、worker handoff、fallback | `src/optimizer/daily_deadline_policy.cpp` |
| OfficialScore、探索、距離helper利用 | `src/optimizer/optimizer.cpp` |
| baselineのGreedy planner、距離helper利用 | `src/planner/greedy_planner.cpp` |
| 補給・rendezvous planner | `src/planner/refuel_planner.cpp` |
| 共通の偶数行右ずれ六角形距離 | `include/hexa_udon/core/hex_distance.hpp` |
| 行動妥当性と結果の単一判定源 | `src/simulator/` |
| LAN workerのrequest/reply | `src/app/lan_worker.cpp` |
| HTTP、rate limit、Retry-After、request control | `src/protocol/` |
| Session、Recovery、polling、永続化 | `src/session/` |

## 13. 未検証事項・制約

- 実LAN、実API、実token、競技POSTは未検証です。loopback、fixture、ローカルbuildの結果から本番完走を推定しません。
- workerが利用できない場合はmain単独のbaseline-first経路へ戻ります。workerは本番必須ではありません。
- type選択後に403となる場合、既存の安全な分類、Retry-After、bounded backoff、deadline停止に従います。複数回POSTの自動再送は未実装で、結果不明はRecoveryRequiredです。
- stdout過多、type選択後の403/backoff、複数回POST未実装は、現在の安全停止契約を壊さない範囲での残課題です。前二者は運用監視上の本番前推奨、複数回POSTは将来改善として扱います。
- 詳細な運用境界は[運用手順](docs/operations.md)、[LAN worker運用](docs/lan-worker.md)、[安全契約](docs/safety.md)、[Recovery](docs/recovery.md)、[phase制約](docs/phase-constraints.md)、[phase status](docs/phase-status.md)を参照します。
