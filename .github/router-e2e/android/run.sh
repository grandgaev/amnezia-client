#!/bin/bash
set -euo pipefail

app_apk=$1
test_apk=$2
companion_apk=$3
artifacts=$4
expected_abi=${ANDROID_E2E_ABI:-}

mkdir -p "$artifacts"
adb logcat -c

if [[ -n "$expected_abi" ]]; then
    device_abis=$(adb shell getprop ro.product.cpu.abilist | tr -d '\r')
    case ",$device_abis," in
        *",$expected_abi,"*) ;;
        *)
            echo "Device ABIs '$device_abis' do not include '$expected_abi'" >&2
            exit 1
            ;;
    esac
    unzip -Z1 "$app_apk" | grep -Fx "lib/$expected_abi/libwg-go.so" >/dev/null
fi

cleanup() {
    adb shell am force-stop org.amnezia.vpn >/dev/null 2>&1 || true
    adb shell am force-stop org.amnezia.routere2e.companion >/dev/null 2>&1 || true
    adb logcat -d -v threadtime \
        -s RouterE2E:V AmneziaVpnService:I Wireguard:I AmneziaWG/Protect:I AndroidRuntime:E \
        > "$artifacts/android-router-e2e-logcat.txt" 2>&1 || true
}
trap cleanup EXIT

adb install -r "$companion_apk"
adb install -r "$app_apk"
adb install -r "$test_apk"
adb shell appops set org.amnezia.vpn ACTIVATE_VPN allow
adb shell pm grant org.amnezia.vpn android.permission.POST_NOTIFICATIONS || true

timeout 8m adb shell am instrument -w -r \
    -e class org.amnezia.routere2e.RouterE2ETest \
    org.amnezia.routere2e/androidx.test.runner.AndroidJUnitRunner \
    | tee "$artifacts/android-router-e2e.txt"

grep -Fq 'OK (1 test)' "$artifacts/android-router-e2e.txt"
