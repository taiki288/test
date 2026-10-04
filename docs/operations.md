# v2 production 当日運用

主PC単独、一度に起動するmain clientは一プロセスだけです。運営のLAN案内から実会場base URLを受け取ってください。仮の本番URLをコードや設定へ固定しません。tokenは環境変数からのみ与え、保存・表示・commit・チャット共有しません。[認証管理](safety.md)と[Recovery](recovery.md)を参照してください。

## Profileの明示指定

|盤面|profile path|agents|許可補給台数|候補数|
|---|---|---:|---|---:|
|16×16|`config/profiles/16x16-one-supply-v2.json`|4|[1]|4|
|24×24|`config/profiles/24x24-two-supply-v2.json`|5|[2]|10|
|32×32|`config/profiles/32x32-one-or-three-supply-v2.json`|7|[1,3]|42|

対応サイズのv2は従来どおり`--profile`で明示指定できます。練習場などで一度だけ起動する場合は`--profile-set v2`を指定すると、最初の認証済み`GET /setting`のsize/agent数に一致する上表のprofileを一意に選びます。このdispatchは同じsetting応答を既存autoへ渡し、追加の`/setting`を発行しません。32×32の2台候補はありません。明示type・復元済み受理typeの既存優先順位を維持します。profile未指定の既定動作は変更しません。

## 任意のLAN worker

主PC単独が標準です。改善候補を別PCで計算する場合だけ、private LANの明示listen addressでworkerを起動し、専用secretを公式tokenとは別の環境変数から渡します。workerは公式APIへ接続せず、主PCから現在日のcanonical PlannerInputだけを受けます。

```bash
export HEXA_LAN_WORKER_SECRET='公式tokenとは別の値'
./hexa_udon worker --listen 192.168.1.20:40123 --worker-token-env HEXA_LAN_WORKER_SECRET
./hexa_udon auto --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile-set v2 --planner daily-improvement \
  --lan-worker 192.168.1.20:40123 --lan-worker-timeout-ms 250
```

主PCはbaselineを先にstrict検証し、worker応答も独立検証します。timeout、切断、identity不一致、reserve不足ではworkerを待たず`baseline-retained`へ戻ります。worker未指定時の既存動作は変わりません。loopbackで確認済みですが、実LAN・実APIは第49段階で確認予定です。wire境界は[LAN運用](lan-worker.md)に記載しています。

## 初日と日次の情報境界

初日PreMatch selectorは認証済みDay0 `/setting`のmap、初期agent位置、燃料上限、spot、サイズだけを使い、全道路Smoothを監査します。未来snapshotを使いません。type提出後は固定typeを保持し、毎日のGETで受けた現在dayの道路snapshot・位置・燃料とprogressから新しく再計画します。前日のplanを翌日に提出しません。

Day1+のoffline研究snapshot、Catalog、実験結果はproduction pathに存在せず、本番PreMatchSelectorや本番Plannerに未来情報を入力しません。profileの承認metadataから実験mapをロードすることもありません。

## 実deadline・baseline-first

|size|baseline最大|improvement最大|Simulator/提出reserve|
|---|---:|---:|---:|
|16|1秒|10秒|8秒|
|24|2秒|20秒|8秒|
|32|5秒|30秒|10秒|

当日の`endsAt`とwall clockの差を一度monotonic deadlineへ変換します。固定60秒を実deadlineにはしません。baseline Greedy/Refuelは共通deadlineを使い、`min(configured上限, 実残時間−reserve)`で制限されます。profileの内部100/150ms値と日次wall-clock上限は別契約です。baseline最大枠は待機時間ではなく、早く完了すれば直ちにstrict Simulator検証へ進みます。

32×32もbaselineを最優先し最大5秒枠で構築します。strict検証済みbaselineがある場合だけ、reserve後に最低250msの探索余剰があれば改善を開始します。改善deadlineも`min(上表の上限, baseline完了後の実残時間−reserve)`です。30秒を必ず使い切る設計ではありません。

OfficialScore辞書順の厳密改善、または3項目完全同点でDailyReadiness厳密改善の場合だけ採用します。Optimizer deadline/失敗、候補Simulator失敗、低scoreは同日検証済みbaselineを`baseline-retained`として保持します。baseline失敗、期限切れ、reserve到達では安全停止し、Wait fallback・前日plan・synthetic successを作りません。deadline確認は協調的であり、OS停止や未知の通信遅延を含む時間内完走を保証しません。

Session/logにpolicy/profile identity、configured上限とclamp後stage上限、開始・baseline完了・改善後の残時間、採否と失敗理由を記録します。`finalRemainingMs`は計画終了時の値で、開始残時間ではありません。

## 参考コマンド（今回実行しない）

base URLは運営から受け取った非秘密の`VENUE_BASE_URL`、tokenは安全に入力した`PROCON_TOKEN`を環境に用意します。以下は接続を伴う参考手順で、package検証では実行しません。

```bash
../30013-prod-build/hexa_udon auto --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile config/profiles/32x32-one-or-three-supply-v2.json \
  --session-dir /secure/runtime/match-session --log-dir /secure/runtime/match-log
```

executeなしはdry-runです。本番提出を明示承認したときだけユーザーが`--execute`を追加します。これは`/agent`と行動POSTを有効にするため、単なるテスト用flagではありません。
