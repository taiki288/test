# Production operation notes

本番クライアントは主PC一台で動かし、tokenは環境変数からのみ読み込みます。試合前は対応サイズのprofileを明示し、Day0の`/setting`だけでPreMatch typeを選びます。各日は新しい観測状態を読み、Greedy-refuel計画をstrict Simulatorで検証してから送信候補にします。通信失敗、状態不整合、Planner失敗、Simulator失敗、deadline不足時は安全停止し、前日の計画や未来snapshotを代用しません。

競技提出の`--execute`、公式サーバー接続、tokenのファイル保存は、この資料の生成・検証では行いません。
