# 認証と秘密管理

認証tokenは`--token-env PROCON_TOKEN`等でプロセス環境から与えます。ソース、profile、設定JSON、Session、log、fixture、Git、標準出力、チャットへ平文を保存・共有しません。shell historyへtokenを直接書かず、terminalの安全な非表示入力等で環境へ用意してください。環境変数一覧をログへ出さないでください。

実会場のbase URLは運営LAN案内で受け取ります。仮の公式URLを固定しません。URLにtokenを付けず、既存clientの認証header経路を使います。Session/logはaccessを制限したGit外のdirectoryに保存します。

package生成、CMake configure/build、`--help`、`validate-profile`はtokenを必要とせず、通信・POSTを行いません。生成manifestも環境変数、token、Authorization、remote URL、絶対home pathを保存しません。client実行時の通信は別の明示操作です。`--execute`は競技POSTを有効にするため、生成・検証で付けません。
