# 有線LAN worker候補計算

LAN workerは`auto`の`--lan-worker HOST:PORT`指定時だけ候補計算補助として動作します。公式API、公式token、base URL、POSTは主PCだけが扱い、workerは現在日のcanonical PlannerInputを受け取って候補を返します。worker未指定時のbaseline-first経路は変わりません。

## 3ターミナル構成

worker 0とworker 1は同じ`build-release/hexa_udon`、同じworker専用secret、同じprotocol schemaを使います。RUN_IDは主PCとworker logで同じ値を使います。実LANではloopback addressを運営LANのprivate addressへ置き換えます。

3ターミナルで同じRUN_IDを手入力または安全な環境変数共有で設定してください。各タブで異なる時刻から生成すると、worker logの突合ができません。

### タブ1: worker 0

```bash
cd ~/30013-port
export HEXA_LAN_WORKER_SECRET='(worker専用secret)'
export RUN_ID='practice-YYYYMMDD-HHMMSS'
mkdir -p "$HOME/hexa-runtime/worker-log"

stdbuf -oL -eL ./build-release/hexa_udon worker \
  --listen 127.0.0.1:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 0 --worker-count 2 \
  --run-id "$RUN_ID" \
  --worker-log "$HOME/hexa-runtime/worker-log/$RUN_ID-worker-39001.jsonl"
```

### タブ2: worker 1

```bash
cd ~/30013-port
export HEXA_LAN_WORKER_SECRET='(worker専用secret)'
export RUN_ID='practice-YYYYMMDD-HHMMSS'
mkdir -p "$HOME/hexa-runtime/worker-log"

stdbuf -oL -eL ./build-release/hexa_udon worker \
  --listen 127.0.0.1:39002 \
  --worker-token-env HEXA_LAN_WORKER_SECRET \
  --worker-index 1 --worker-count 2 \
  --run-id "$RUN_ID" \
  --worker-log "$HOME/hexa-runtime/worker-log/$RUN_ID-worker-39002.jsonl"
```

### タブ3: 主PC

主PCではworkerと同じsecretを設定し、毎回新しいSession/log directoryを使います。

```bash
cd ~/30013-port
export VENUE_BASE_URL='https://<practice-venue-host>'
export PROCON_TOKEN='<practice-token>'
export HEXA_LAN_WORKER_SECRET='(worker専用secret)'

RUN_ID="practice-$(date +%Y%m%d-%H%M%S)"
export PRACTICE_SESSION="$HOME/hexa-runtime/$RUN_ID/session"
export PRACTICE_LOG="$HOME/hexa-runtime/$RUN_ID/log"
mkdir -p "$PRACTICE_SESSION" "$PRACTICE_LOG"

ss -ltnp | grep -E ':39001|:39002'
./build-release/hexa_udon worker-preflight --listen 127.0.0.1:39001 \
  --worker-token-env HEXA_LAN_WORKER_SECRET --worker-index 0 --worker-count 2
./build-release/hexa_udon worker-preflight --listen 127.0.0.1:39002 \
  --worker-token-env HEXA_LAN_WORKER_SECRET --worker-index 1 --worker-count 2
```

両workerが`worker=ready`を表示し、両portがLISTENで、preflightがprotocol schema、build fingerprint、logical worker index/count、evaluator/profile identity、secret設定済みを確認できてから主PCを起動します。

```bash
./build-release/hexa_udon auto \
  --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement --execute \
  --lan-worker 127.0.0.1:39001 --lan-worker 127.0.0.1:39002 \
  --lan-worker-timeout-ms 30000 \
  --session-dir "$PRACTICE_SESSION" --log-dir "$PRACTICE_LOG"
```

## timeout・fallback・診断

- worker capは16×16=5000ms、24×24=10000ms、32×32=15000ms。CLIの上限は30000msで、60000msは指定できません。
- `fallback`、`readiness-lost`、`deadline-exhausted`、`strict-failure`はtransport failureではありません。
- transport failureはconnect/write/read/EOF/auth/protocol/frameの通信失敗です。deadline到達は`deadline-exhausted`として扱い、socket read timeoutとは分離します。
- timeout worker、preflight失敗、claim mismatch、strict failureはそのworkerだけを不採用にし、mainまたはbaselineへfallbackします。
- worker logにはRUN_ID、worker index、phase、failure classification、elapsedMsだけを保存します。token、secret、digest実値、HTTP本文、action配列、不要なmap/profile内容は保存しません。
- Session/log/worker logはGit外の保護directoryで管理し、directoryは0700、ファイルは0600で作成します。
- `RecoveryRequired`後に同一Sessionでautoを再実行しません。`show-state`と`recover`のdry-runで人手確認します。
- 実LAN、実API、実token、競技POSTはこの手順の検証範囲外です。
