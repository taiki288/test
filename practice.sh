#!/usr/bin/env bash

set -o pipefail
mkdir -p run

if [[ -z ${PROCON_TOKEN:-} ]]; then
  echo "PROCON_TOKEN is not set." >&2
  exit 1
fi

echo "Configuring and building Release client..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release || exit $?
cmake --build build --parallel 4 || exit $?

# you can custom waiting time(limited 180)
echo "Waiting for the match setting (up to 120 seconds)..."
setting_deadline=$((SECONDS + 120))
while :; do
  : > run/setting-response.json
  http_status=$(curl -sS \
    --connect-timeout 5 \
    --max-time 10 \
    -o run/setting-response.json \
    -w '%{http_code}' \
    -H "Procon-Token: ${PROCON_TOKEN}" \
    "https://procon37arena.online/setting")
  curl_status=$?

  if [[ $curl_status -ne 0 ]]; then
    echo "Setting request failed (curl status $curl_status)." >&2
  elif [[ $http_status == 200 ]]; then
    echo "Match setting is available; starting client."
    break
  elif [[ $http_status == 401 ]]; then
    echo "Authentication failed. Check PROCON_TOKEN." >&2
    exit 1
  elif [[ $http_status != 403 ]]; then
    echo "Unexpected /setting response: HTTP $http_status" >&2
    cat run/setting-response.json >&2
    exit 1
  fi

  if (( SECONDS >= setting_deadline )); then
    echo "Timed out waiting for the match setting." >&2
    if [[ -s run/setting-response.json ]]; then
      cat run/setting-response.json >&2
    fi
    exit 1
  fi
  sleep 1
done

run_client() {
  ./build/hexa_udon auto \
    --base-url "https://procon37arena.online" \
    --token-env PROCON_TOKEN \
    --profile-set v2 \
    --planner daily-improvement \
    --execute \
    2>&1 | tee run/client-output.log
}

run_client
client_status=$?

if [[ $client_status -ne 0 ]] \
  && grep -Fq "persisted state belongs to another match" run/client-output.log \
  && [[ -d run/session ]]; then
  if [[ -f run/session/session.json ]] \
    && { grep -Eq '"agentKindsUnknown"[[:space:]]*:[[:space:]]*true' run/session/session.json \
      || grep -Eq '"classification"[[:space:]]*:[[:space:]]*5([,}[:space:]]|$)' run/session/session.json; }; then
    echo "Previous match state has an unknown POST outcome; not archiving automatically." >&2
    exit "$client_status"
  fi

  archive="run/session-previous-$(date +%Y%m%d-%H%M%S)"
  suffix=1
  while [[ -e "$archive" ]]; do
    archive="run/session-previous-$(date +%Y%m%d-%H%M%S)-$suffix"
    ((suffix += 1))
  done

  mv run/session "$archive"
  echo "Previous match state archived to $archive; retrying."
  run_client
  client_status=$?
fi

exit "$client_status"
