# 認証と秘密管理

認証tokenは`--token-env PROCON_TOKEN`等でプロセス環境から与えます。ソース、profile、設定JSON、Session、log、fixture、Git、標準出力、チャットへ平文を保存・共有しません。shell historyへtokenを直接書かず、terminalの安全な非表示入力等で環境へ用意してください。環境変数一覧をログへ出さないでください。

実会場のbase URLは運営LAN案内で受け取ります。仮の公式URLを固定しません。URLにtokenを付けず、既存clientの認証header経路を使います。Session/log/worker logはRUN_ID単位のGit外directoryに保存し、directoryは0700、Session JSON・OperationLog・worker logは0600で作成します。stdoutとOperationLogにはaction全配列を保存しませんが、Recoveryに必要なactionは保護されたSession stateへ保存します。Session/log/worker logはGit管理対象・提出対象外です。

GitHub mainからのclean build、`--help`、`validate-profile`、CTestはtokenを必要とせず、通信・POSTを行いません。旧package manifestとmanifest hashは現行実行系・提出経路では使用しません。提出対象にはruntime、results、build成果物、token、secret、Session/logを含めません。`~/30013-submit`は旧production package工程であり、現行提出経路では使用しません。client実行時の通信は別の明示操作です。`--execute`は競技POSTを有効にするため、生成・検証で付けません。

worker 0/1/mainは同じ`build-release/hexa_udon`と同じworker secretを使います。`worker-preflight`はlisten、protocol schema、build fingerprint、logical worker index/count、evaluator/profile identity、secret設定済みだけを確認し、secret値、digest実値、HTTP本文、action配列を表示しません。実LAN、実API、実token、競技POSTは未検証です。
