#!/usr/bin/env bash
# Bruce firmware patch smoke test. Run against a flashed smoochiee-board on the
# same Wi-Fi as your workstation (or its AP). Set BRUCE_URL + BRUCE_TOKEN.
# Usage: BRUCE_URL=http://bruce.local BRUCE_TOKEN=<token> bash tools/smoke_test_attacks.sh
set -euo pipefail
: "${BRUCE_URL:?need BRUCE_URL}"
: "${BRUCE_TOKEN:?need BRUCE_TOKEN}"

auth="Authorization: Bearer ${BRUCE_TOKEN}"
pass=0; fail=0
chk() {
    local name="$1"; local url="$2"; local want="${3:-200}"
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "$auth" "$url")
    if [ "$code" = "$want" ]; then
        echo "PASS  $name -> $code"; pass=$((pass+1))
    else
        echo "FAIL  $name -> got $code want $want"; fail=$((fail+1))
    fi
}

echo "== Existing endpoints (regression) =="
chk "GET  /systeminfo"    "${BRUCE_URL}/systeminfo"
chk "GET  /getscreen"     "${BRUCE_URL}/getscreen"
chk "GET  /listfiles"     "${BRUCE_URL}/listfiles?fs=LittleFS"

echo "== New attack verbs (HTTP 200 = queued) =="
chk "POST ble api on"     "${BRUCE_URL}/cm?cmnd=ble%20api%20on"
chk "POST evilportal"     "${BRUCE_URL}/cm?cmnd=evilportal%20FreeWifi%206"
sleep 3   # let evil portal spin up — comment out if you don't have a victim device ready
chk "POST blespam fastpair" "${BRUCE_URL}/cm?cmnd=blespam%20fastpair_regular%203"
chk "POST karma"          "${BRUCE_URL}/cm?cmnd=karma"
chk "POST deauth"         "${BRUCE_URL}/cm?cmnd=deauth"
chk "POST blesniffer"     "${BRUCE_URL}/cm?cmnd=blesniffer"
chk "POST ap_info"        "${BRUCE_URL}/cm?cmnd=ap_info"
chk "POST reverseshell"   "${BRUCE_URL}/cm?cmnd=reverseshell"
chk "POST pwngrid"        "${BRUCE_URL}/cm?cmnd=pwngrid"
chk "POST ble api off"    "${BRUCE_URL}/cm?cmnd=ble%20api%20off"

echo "== Negative auth =="
curl -s -o /dev/null -w 'no-auth    -> %{http_code}\n' "${BRUCE_URL}/systeminfo"
curl -s -o /dev/null -w 'bad-bearer -> %{http_code}\n' -H "Authorization: Bearer wrong" "${BRUCE_URL}/systeminfo"

echo "== Summary =="
echo "PASS=$pass  FAIL=$fail"
exit $((fail > 0))
