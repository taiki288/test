# 有線LAN worker候補計算

LAN workerは明示的な`--lan-worker HOST:PORT`指定時だけ有効です。主PCが公式API、token、base URL、POST、baseline生成、最終Simulator検証、候補採否を保持します。workerにはcanonical化した現在日のsetting/map、DailyState、progress、固定type、policy/identity digest、相対budgetだけを渡し、未来snapshot・公式認証情報・競技HTTPは渡しません。

workerは専用共有secretを環境変数から読みます。競技tokenとは別の環境変数を使い、secretをログ、Session、JSON、画面へ出しません。listen addressはloopbackまたは明示したprivate LAN addressだけにし、公開addressや`0.0.0.0`を使いません。

```text
HEXA_LAN_WORKER_SECRET='(安全な環境変数)' ./build/hexa_udon worker --listen 127.0.0.1:40123 --worker-token-env HEXA_LAN_WORKER_SECRET
HEXA_LAN_WORKER_SECRET='(安全な環境変数)' ./build/hexa_udon auto --profile-set v2 --planner daily-improvement --lan-worker 127.0.0.1:40123
```

workerのtimeout、切断、認証・version・identity不一致、Simulator不一致、reserve不足では、主PCはworkerを待たず検証済みbaselineを`baseline-retained`として使います。worker replyは主PCでstrict Simulator再検証し、OfficialScoreの厳密改善または完全同点のReadiness改善だけを採用します。worker未指定時は既存の日次経路を使います。

完全なPlannerInputが揃わないrequestはworker側で構造化拒否します。合成scoreや未検証planを返さず、主PCのbaselineを維持する安全側の契約です。

この経路はloopback/private-LAN候補計算のためのもので、本番接続・公式POST・`--execute`を今回実行していません。
