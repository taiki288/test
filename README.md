# hexa-udon

16×16、24×24、32×32のv2 profileに対応するC++20 clientです。WSL/Linuxでの実行を基本とし、練習場と本番は`--base-url`で接続先を分けます。`--execute`を付けた場合だけ提出POSTを有効にします。

主PCが公式API、token、baseline生成、strict Simulatorによる最終検証、提出を担当します。LAN workerは任意の候補計算補助で、timeout、通信失敗、claim mismatch、strict再検証失敗時はbaselineへfallbackします。

## 初期セットアップ

必要なものはC++20対応コンパイラ、CMake 3.20以上、nlohmann_json 3.11.3以上、libcurlです。CMakeは依存関係をネットワークから取得しません。build directoryは構成ごとに分け、同じdirectoryをDebug/ReleaseやOS間で共用しないでください。

```bash
# Debug
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel 4

# Release
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel 4

# CLI確認（通信なし）
./build-release/hexa_udon --help
```

v2 profileは次の3種類です。`validate-profile`はoffline検証で、tokenやHTTPを必要としません。

```bash
./build-release/hexa_udon validate-profile --profile config/profiles/16x16-one-supply-v2.json
./build-release/hexa_udon validate-profile --profile config/profiles/24x24-two-supply-v2.json
./build-release/hexa_udon validate-profile --profile config/profiles/32x32-one-or-three-supply-v2.json
```

秘密情報は実値をREADMEやGitへ書かず、環境変数から渡します。

```bash
export VENUE_BASE_URL='(安全な環境で設定)'
export PROCON_TOKEN='(安全な環境で設定)'
export HEXA_LAN_WORKER_SECRET='(公式tokenとは別に安全な環境で設定)'
```

`runtime/`、`results/`、Session、operations log、token、worker secret、build成果物はcommitしません。

## CLIと起動方法

### check

```bash
./build-release/hexa_udon check --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN
```

`check`は成功/失敗と安全な分類だけを表示し、base URL、host、token、HTTP本文は表示しません。

### auto

通常はdry-runです。dry-runでもGETによる観測は行いますが、競技POSTは行いません。

```bash
./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log
```

`--execute`は明示的に提出POSTを有効にするオプションです。

```bash
./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement --execute \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log
```

`--profile FILE`は`auto`または`recover`に渡せます。`--profile-set v2`は`auto`で最初の`GET /setting`から16×16、24×24、32×32のv2 profileを選択します。32×32の供給車allowlistは`[1,3]`です。

### worker

workerは任意の探索補助機能です。主PC単独でも通常のbaseline-first経路を実行できます。workerを使う場合も、workerの失敗、timeout、通信失敗、claim mismatch、strict再検証失敗では、主PCのbaselineまたは既に得られたmain候補へ安全にfallbackします。worker候補を最終採用するかどうかは、常に主PCが決定します。

workerはloopbackまたはprivate LANの明示addressで起動します。worker専用secretは環境変数から渡し、実値はREADMEやGitへ書きません。

```bash
# loopback例
export HEXA_LAN_WORKER_SECRET='(安全な環境で設定)'
./build-release/hexa_udon worker \
  --listen 127.0.0.1:39001 --worker-token-env HEXA_LAN_WORKER_SECRET

# private LAN例（実際のIP、port、secretは運用環境で設定）
./build-release/hexa_udon worker \
  --listen 192.168.1.20:39001 --worker-token-env HEXA_LAN_WORKER_SECRET
```

主PCからworkerを指定する場合は、`--lan-worker`をrepeatableに指定できます。提出を行う実行例は`--execute`を明示し、SessionとOperationLogはGit外の保護されたdirectoryへ置きます。

```bash
./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement --execute \
  --lan-worker 127.0.0.1:39001 --lan-worker-timeout-ms 10000 \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log
```

private LANでは、主PCから到達できるworker endpointを指定します。

```bash
./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement --execute \
  --lan-worker 192.168.1.20:39001 --lan-worker-timeout-ms 10000 \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log
```

workerはbaselineをstrict検証した後の探索補助を担当し、本体Optimizerと共有hard deadline内で並行探索します。worker 0/1は、worker index/count、requestIdDigest、canonical inputHash、planner seed、候補数などのclaim情報を付けて候補を生成します。主PCはreplyのclaimを比較し、input/state/map/profile identity、seed、index/count、候補hashなどを再確認します。

主PCはworker候補をstrict Simulatorで再検証し、OfficialScoreを比較します。OfficialScoreが同等の場合だけDailyReadinessを比較し、最後にdeterministic tie-breakを適用します。timeout、通信失敗、claim mismatch、strict Simulator失敗、deadline不足の候補は採用せず、baselineまたはmain候補へfallbackします。

workerは公式API、公式base URL、公式tokenを扱いません。workerが読むのはworker専用secretだけで、環境変数から渡します。token、secret、URL、HTTP本文、action全配列を通常ログへ出しません。listen addressはloopbackまたはprivate LANに限定し、公開addressや`0.0.0.0`を使用しません。実LANで使う前に、IP、port、firewall、binary version、主PCからの到達性を確認してください。

今後の改善対象は、workerごとの探索範囲、共有hard deadline内のtimeout配分、worker間の役割分担、候補診断とstdout整理です。これらを変更しても、主PCのstrict再検証とbaseline fallbackを維持します。

### recover / show-state

`recover`は保存済みSessionを使う復旧経路で、`--execute`なしではdry-runです。POST後の応答不明状態はRecoveryRequiredとなり、自動再送しません。

```bash
./build-release/hexa_udon recover \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --session-dir /secure/runtime/session --log-dir /secure/runtime/log

./build-release/hexa_udon show-state --session-dir /secure/runtime/session
```

## アルゴリズム実装場所

| 領域 | 主な実装 |
| --- | --- |
| API sequence、日次実行、候補採否 | `src/app/auto_client.cpp` |
| deadline、baseline、worker handoff、fallback | `src/optimizer/daily_deadline_policy.cpp` |
| OfficialScore、探索、近傍受理 | `src/optimizer/optimizer.cpp` |
| baselineのGreedy planner | `src/planner/greedy_planner.cpp` |
| 補給・rendezvous planner | `src/planner/refuel_planner.cpp` |
| 六角形経路・距離 | `src/pathfinding/` |
| 行動妥当性と結果の単一判定源 | `src/simulator/` |
| LAN workerのrequest/reply | `src/app/lan_worker.cpp` |
| HTTP、rate limit、Retry-After、request control | `src/protocol/` |
| Session、Recovery、polling、永続化 | `src/session/` |

## 採用判定の流れ

```text
baseline生成・strict検証
  → 本体Optimizerとworker 0/1を並行探索
  → 主PCでclaim比較・strict Simulator再検証
  → OfficialScore（辞書順）
  → 同点時DailyReadiness
  → deterministic tie-break
  → 採用、またはbaseline-retained
```

policy層は候補生成、実行順、deadline、timeout/fallback、strict検証済み候補の返却を担当します。App層はworker claimの再検証、主PCstrict結果の確認、OfficialScore/Readiness比較、最終採用を担当します。

## 重要な安全契約

- `hardPlanningDeadline = endsAt - 10秒`。最後の10秒は提出前の通信・安全余裕です。
- `send_at >= deadline`ではGET/POSTを開始しません。deadline後に新しいstrict検証も開始しません。
- policyのreserveは改善開始可否、fallback、診断に使いますが、hard deadlineから二重減算しません。
- worker候補は主PCでinput、state、map/profile identity、termination、score、readinessを再検証します。
- claim mismatch、timeout、transport/protocol failure、strict Simulator失敗の候補は採用しません。
- `submissionAttempted`は`false`（POST前）、`true`（POST後2xx）、`null`（POST後の不明結果）の三値です。
- `null`はRecoveryRequiredとして保存し、自動再送しません。`false`はPOST前停止でありRecoveryRequiredではありません。
- 通常ログへtoken、URL、HTTP本文、action配列、worker secretを保存しません。

API経路は初回`GET /setting`、`POST /agent`（type提出）、type提出後の日次`GET /`、日次`POST /`です。403/429、Retry-After、bounded backoff、polling limit、deadline停止はOperationLogへ安全な分類と数値で記録します。初回settingを日次pollingで再取得しないため、`/setting` stormを作りません。

## テスト

```bash
ctest --test-dir build-debug --output-on-failure
ctest --test-dir build-release --output-on-failure

# 関連6件だけ
ctest --test-dir build-debug -R \
  'app_unit_tests|lan_worker_unit_tests|daily_deadline_policy_tests|control_tests|protocol_tests|session_tests' \
  --output-on-failure
```

CTestには次の6件が登録されています。

- `app_unit_tests`
- `lan_worker_unit_tests`
- `daily_deadline_policy_tests`
- `control_tests`
- `protocol_tests`
- `session_tests`

ASan/UBSanは専用build directoryで構成します。loopback workerはsanitizerや低速環境でtimeoutし得るため、機能失敗と環境由来のtimeoutを分けて報告します。

```bash
cmake -S . -B ../30013-build-asan \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build ../30013-build-asan --parallel 4
ctest --test-dir ../30013-build-asan --output-on-failure
```

```bash
git diff --check
```

## GitHub mainと提出物の運用

GitHub mainは共有repoであり、そのsource treeを現行の提出対象として扱います。実行・提出前は、GitHub mainの`src/`、`include/`、`config/profiles/`、CMake定義、必要なdocsからclean buildを作ります。

旧package manifest、allowlist hash、source commit metadataは現行実行系・提出経路では使用しません。package専用manifestを前提にした旧production package運用は廃止します。

提出対象にはruntime、results、build成果物、Session/log、token、worker secretなどの実行時データや秘密情報を含めません。`runtime/`と`results/`はGitHub mainおよび提出物から分離します。

`~/30013-submit`は旧production package工程の成果物であり、現行のGitHub main提出経路では使用しません。現行mainを直接確認し、sourceからclean buildして提出前検証を行います。

## 関連文書

- [運用手順](docs/operations.md)
- [LAN worker運用](docs/lan-worker.md)
- [Recovery](docs/recovery.md)
- [安全契約](docs/safety.md)
