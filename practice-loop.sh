#!/usr/bin/env bash
# 練習場の試合に出続ける。practice.sh を繰り返し呼ぶ（Ctrl+C で止める）。
#   PROCON_TOKEN=... ./practice-loop.sh                     # 既定の設定で
#   PROCON_TOKEN=... ./practice-loop.sh --threads 8         # 引数は hexa_udon auto に渡る
# 1 試合ごとの client の出力は run/client-output-<時刻>.log に残す。
# 前の試合の Session は practice.sh が run/session-previous-<時刻> に退避する。

set -o pipefail
mkdir -p run

if [[ -z ${PROCON_TOKEN:-} ]]; then
  echo "PROCON_TOKEN is not set." >&2
  exit 1
fi

echo "Configuring and building Release client..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release || exit $?
cmake --build build --parallel 4 || exit $?

while :; do
  PRACTICE_SKIP_BUILD=1 ./practice.sh "$@"
  status=$?
  [[ -f run/client-output.log ]] && mv run/client-output.log "run/client-output-$(date +%Y%m%d-%H%M%S).log"
  case $status in
    0) echo "Match finished. Waiting for the next match..." ;;
    3) ;;  # 試合の設定がまだ出ていないので待ち直す
    130) echo "Stopped." >&2; exit 130 ;;
    *)
      # 認証エラー、結果の分からない POST が残った Session、RecoveryRequired などは人が確かめる
      echo "practice.sh exited with status $status; stopping the loop for manual inspection." >&2
      exit "$status"
      ;;
  esac
done
