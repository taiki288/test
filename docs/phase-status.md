# 現行main phase status

## 現在の状態

現行mainは、v2 production source、baseline-firstの日次計画、共有hard deadline、任意LAN workerの並行候補計算、claim比較とstrict再検証、OfficialScore/DailyReadiness/deterministic tie-break、POST三値とunknown時停止を実装済みである。CTest 6件はCMakeへ登録済みである。

workerは公式APIや公式tokenを扱わず、requestIdDigestを主PCと共通化する。worker timeout、deadline不足、identity不一致、strict再検証失敗時はbaselineへfallbackする。

## 未完了・別工程

- 実LAN、実API、実token、競技POSTは未検証。
- 同一POSTの複数回自動送信は未実装。unknown後の自動再送は禁止。
- package再生成、manifestのallowlist/SHA-256/source基準確認は別工程。
- stdoutの整理、type選択後の403/backoff/timeout時系列診断は後続課題。
- tuning、dashboard、replay、runtime、resultsはproduction sourceと分離する。

## 今回のphaseの成果物整理

Git生成物は`.gitignore`で除外し、tracked build成果物はGit管理から外す。ローカルbuild実体とruntime/results実体は削除・移動しない。`PACKAGE-MANIFEST.json`は変更しない。

## 次の工程

1. cleanなpackage生成環境でproduction allowlistとmanifestを確認する。
2. stdoutを最小化し、診断情報をOperationLogへ集約する。
3. type選択後から最初の日次GETまでの通信時系列を診断可能にする。
4. 実LAN・実APIの扱いは、別途明示承認と安全な検証計画が整った場合だけ判断する。
