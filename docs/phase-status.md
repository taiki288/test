# 現行main phase status

## 現在の状態

現行mainは、v2 production source、baseline-firstの日次計画、共有hard deadline、任意LAN workerの並行候補計算、claim比較とstrict再検証、OfficialScore/DailyReadiness/deterministic tie-break、POST三値とunknown時停止を実装済みである。CTest 6件はCMakeへ登録済みである。

workerは公式APIや公式tokenを扱わず、requestIdDigestを主PCと共通化する。worker timeout、deadline不足、identity不一致、strict再検証失敗時はbaselineへfallbackする。

## 未完了・別工程

- 実LAN、実API、実token、競技POSTは未検証。
- 同一POSTの複数回自動送信は未実装。unknown後の自動再送は禁止。
- GitHub mainを共有repo兼提出対象とし、実行・提出前はsourceからclean buildを作る。
- package専用manifestは現行実行系・提出経路では使用しない。`~/30013-submit`は旧production package工程であり、現行提出経路では使用しない。
- stdoutは人間向けの簡潔な状態・進捗表示とし、詳細診断はOperationLog/Sessionへ保存する。type選択後の403/backoff/timeout時系列も診断情報として保存する。
- tuning、dashboard、replay、runtime、resultsはproduction sourceと分離する。

## 今回のphaseの成果物整理

Git生成物は`.gitignore`で除外し、tracked build成果物はGit管理から外す。ローカルbuild実体とruntime/results実体は削除・移動しない。GitHub mainのsource treeを提出対象とするが、runtime、results、build成果物、token、secretは含めない。

## 次の工程

1. GitHub mainからclean buildを作り、提出前のCLI・profile・CTest確認を行う。
2. 実LAN・実APIの扱いは、別途明示承認と安全な検証計画が整った場合だけ判断する。
