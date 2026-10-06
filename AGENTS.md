# 現行mainの作業規約

この文書は、`origin/main`と一致する現行`main`の作業境界を記録する。旧worktree、`~/30013`、stashの文書は履歴・参照用であり、現行mainの状態や制約の根拠にはしない。

## 実装状態

- production sourceは`src/`、`include/`、`config/profiles/`に分離されている。
- 日次経路はbaseline-firstで、まずGreedy/Refuel baselineを生成してstrict Simulatorで検証する。
- 明示指定時のLAN workerは、本体Optimizerと共有hard deadline内で並行dispatchする。workerは候補計算補助であり、公式API、公式token、POSTを扱わない。
- worker requestとreplyの`requestIdDigest`は共通実装を使う。主PCはclaimを比較し、strict Simulatorで再検証する。
- 最終採否はOfficialScore、DailyReadiness、deterministic tie-breakの順で決める。不正、timeout、deadline超過、再検証失敗はbaselineへfallbackする。
- `hardPlanningDeadline`、worker timeout、POST前の安全余裕を共有し、期限後に新規計算・GET・POSTを開始しない。
- `submissionAttempted`は`false`（POST前）、`true`（応答を受理）、`null`（POST後の結果不明）の三値で保存する。unknown後の自動再送は禁止する。
- 同一POSTの複数回自動送信は未実装である。RecoveryRequiredを人手確認の停止点とする。

## 検証と境界

- CTestには`app_unit_tests`、`lan_worker_unit_tests`、`daily_deadline_policy_tests`、`control_tests`、`protocol_tests`、`session_tests`の6件が登録済みである。
- package再生成と`PACKAGE-MANIFEST.json`の最終確認は、source変更とは別工程で行う。manifestはその工程以外で変更しない。
- 実LAN、実API、実token、競技POSTは未検証であり、ローカルfixtureやloopbackの確認結果から本番完走を推定しない。
- tuning、dashboard、replay、runtime、resultsはproduction sourceと分離する。生成物、秘密情報、Session/logをsourceへ追加しない。

## 作業ルール

- `src/`、`include/`、production profile本体、protocol/session本体を、文書・生成物整理の作業で変更しない。
- build、CTest、通信、token、競技POSTは、目的と対象を明示した作業でのみ実行する。
- build directory、CMake生成物、`runtime/`、`results/`はGit管理対象外とする。
- 既存のruntime/results実体は、整理作業で削除・移動しない。
