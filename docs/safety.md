# 認証と秘密管理

認証tokenは`--token-env PROCON_TOKEN`等でプロセス環境から与えます。ソース、profile、設定JSON、Session、log、fixture、Git、標準出力、チャットへ平文を保存・共有しません。shell historyへtokenを直接書かず、terminalの安全な非表示入力等で環境へ用意してください。環境変数一覧をログへ出さないでください。

実会場のbase URLは運営LAN案内で受け取ります。仮の公式URLを固定しません。URLにtokenを付けず、既存clientの認証header経路を使います。Session/logはaccessを制限したGit外のdirectoryに保存します。

GitHub mainからのclean build、`--help`、`validate-profile`、CTestはtokenを必要とせず、通信・POSTを行いません。旧package manifestとmanifest hashは現行実行系・提出経路では使用しません。提出対象にはruntime、results、build成果物、token、secret、Session/logを含めません。`~/30013-submit`は旧production package工程であり、現行提出経路では使用しません。client実行時の通信は別の明示操作です。`--execute`は競技POSTを有効にするため、生成・検証で付けません。
