# 現行main phase constraints

## 適用範囲

対象は`/home/yoshikawar/30013-port`の現行`main`だけとする。`~/30013`、旧worktree、stashの文書は参照用であり、現行phaseの制約を上書きしない。

## 現行実装の契約

- baseline-firstを維持し、baselineのstrict検証済み結果を常に保持する。
- 明示opt-inのworkerは、本体Optimizerと並行dispatchする。workerの候補はrequestIdDigest、identity、claimを主PCで比較し、strict Simulatorで再検証する。
- OfficialScoreを第一比較、DailyReadinessを同点時の比較、deterministic tie-breakを最終決定に使う。
- hard deadlineをbaseline、worker、Optimizer、strict再検証、提出前余裕で共有する。worker timeout、deadline不足、通信失敗、claim不一致、再検証失敗ではbaselineへ戻す。
- POST結果は`false`、`true`、`null`の三値で扱う。`null`はunknownとしてRecoveryRequiredにし、自動再送しない。同一POSTの複数回自動送信は実装しない。

## 禁止・未検証事項

- 実LAN、実API、実token、公式サーバー、競技POST、`--execute`をこのphaseの検証対象にしない。
- tuning、dashboard、replay、runtime、results、実験成果物をproduction sourceへ混在させない。
- package再生成とmanifest確認は別工程とし、通常の文書・Git生成物整理では`PACKAGE-MANIFEST.json`を変更しない。
- source、include、config、docs、tests、scriptsをbuild生成物整理の対象にしない。
