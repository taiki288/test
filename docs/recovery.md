# 安全停止とRecovery

通信失敗、状態不整合、baseline Planner/strict Simulator失敗、deadline不足では安全停止します。応答不明のPOSTを自動再送せず、前日planや未来snapshotを代用しません。

停止・再起動時は同じSession directory、元と同じv2 profile、固定type、同じ試合設定を使います。二つのmain clientを同時に動かさないでください。Session/profile/policy/size/type identity不一致は停止条件です。v1 Sessionを暗黙にv2へ移行しません。Session/log/worker logはRUN_ID単位のGit外の保護された場所へ置き、directoryは0700、ファイルは0600で作成し、profileやsource更新時に削除しません。

次は運営から接続先を受け取り、復旧判断後にユーザーが使う参考手順です。今回実行しません。

```bash
./build-release/hexa_udon recover --base-url "$VENUE_BASE_URL" --token-env PROCON_TOKEN \
  --profile config/profiles/32x32-one-or-three-supply-v2.json \
  --session-dir /secure/runtime/match-session --log-dir /secure/runtime/match-log
```

`recover`もexecuteなしはGET専用dry-runです。提出再開は明示承認後にだけ`--execute`を付けます。dry-runの検証receiptは公式受理dayとは分離され、action/type/state/snapshot identityを確認して復元します。公式に受理されたかを推測して履歴を補完しません。

`RecoveryRequired`後に同じSessionで`auto`を再実行しません。`show-state`、Session、公式状態を人手で照合し、必要なら`recover`のdry-runから再開判断を行います。同一POSTの自動再送は禁止です。worker preflight失敗やworker全台失敗はmainのbaseline fallbackとは別に記録し、再送理由にはしません。

packageはproduction sourceだけです。第43段階の開発repo側リハーサルでSession/Recovery契約を確認済みですが、それは会場通信や本番deadline内完走の保証ではありません。
