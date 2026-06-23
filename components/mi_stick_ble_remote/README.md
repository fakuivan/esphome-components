# Mi Stick BLE Remote

ESPHome BLE HID experiments for waking and controlling a Xiaomi Mi TV Stick.

This component is still a research/prototype component. The main risk is not
only sending the right HID report; it is finding a pairing mode that is useful
in normal life, where the stock Xiaomi remote must remain paired and usable.

## Current Shape

The component currently supports two HID profiles:

| Profile | Intent | Current status |
| --- | --- | --- |
| `xiaomi_rc` | Emulate the stock Xiaomi BLE remote closely enough for the TV's Xiaomi remote service. | Confirmed to pair and wake when it owns the remote slot. Not acceptable as the final path if it prevents the stock remote from also working. |
| `generic_linux` | Use a small Android/Linux HID payload. | Current active path. It is advertised under the Xiaomi-accepted `MI RC` identity so the Mi Stick should save the ESP MAC as wake-capable, while avoiding the `Xiaomi RC` Realtek/audio remote branch. |

The active ESPHome YAML should be treated as the test fixture for the current
experiment, not as a final recommendation.

## Known Evidence

### Experimental Findings

- The stock Xiaomi remote's wake-capable Home press was observed as a Consumer
  Control report with payload `00 00 04`.
- The stock Xiaomi remote's Power press was observed as payload `20 00 00`.
- The `xiaomi_rc` profile became much more reliable after using explicit Report
  ID `1` in the HID descriptor and when sending reports.
- A `xiaomi_rc` ESP identity using name `Xiaomi RC`, vendor ID `0x2717`, product
  ID `0x32B9`, and static random address `D4:1F:E8:2B:71:7E` was observed as a
  bonded input device and could wake the stick.
- The active `generic_linux` profile advertises as `MI RC` but uses a minimal
  generic HID input report. It paired automatically after the stock remote was
  made unavailable by removing its battery, without deleting the stock remote's
  bond from the Mi Stick.
- The physical Xiaomi remote can remain paired while the ESP `MI RC` device is
  also paired. This is the first tested path that appears compatible with normal
  use.
- Generic Desktop System Wake Up works as the best wake command found so far. It
  wakes the Mi Stick without also navigating to Android TV Home.
- Generic Desktop System Sleep did not put the Mi Stick to sleep in testing.
  Keep it as a harmless experimental control, but do not treat it as proven.
- HID Power remains a fallback toggle rather than an explicit off command.

### Reverse-Engineering Findings

- Static analysis of the matching upgrade image showed that this build's Xiaomi
  Bluetooth service classifies remotes by names from `bt_rc_name.conf`.
  `MI RC` is accepted as a remote name, but unlike `Xiaomi RC`, it does not enter
  the Realtek/audio remote branch.
- The same service only writes wake-capable MACs for devices classified as a
  Xiaomi RC, so a purely generic keyboard/media identity is unlikely to wake the
  stick from standby.
- The auto-pair scanner requires a complete local name matching the Xiaomi
  remote allowlist, HOGP UUID `0x1812`, TX power advertising data, and a close
  enough RSSI/path-loss check unless the address is already the last-used remote.
- The stock remote firmware advertises `Xiaomi RC` as the complete local name
  and `MI RC` as the shortened name. For this ESP component, `MI RC` is used as
  the complete name so Android's scanner can classify it while avoiding the
  `Xiaomi RC` audio branch.
- On HID connection, the Mi Stick stores wake-capable remote MACs in
  `persist.bluetooth.wakeuprcaddr` and the `bt_rc_mac` unify key. The Java path
  was identified; the lower-level consumer of that key was not conclusively
  located.
- Android/Linux input mappings support Generic Desktop System Wake Up as
  `KEY_WAKEUP` / `WAKEUP`, and Generic Desktop System Sleep as `KEY_SLEEP` /
  `SLEEP`. The former works on this Mi Stick; the latter was ignored.
- When the physical Xiaomi remote is already paired, the Xiaomi remote service
  logs `allowMoreThanOneRC:false`. In that state, additional remote-like
  advertisers may be visible to Bluetooth scanning but not offered for pairing.

## Goal Matrix

### Without Stock Remote Connected

These tests isolate HID behavior from the one-remote policy.

| Goal | Desired result | Status |
| --- | --- | --- |
| Stock remote emulation, all buttons | Pair as a Xiaomi-style remote and send the full useful button set. | Partially proven for wake/Home/Power-style reports. Full button map still needs implementation and verification. |
| Generic keyboard emulation | Pair as a normal BLE keyboard/HID device, independent of Xiaomi remote logic. | Not yet tested fairly. Needs a keyboard-like HID descriptor, not only Consumer Control. |
| Purpose-built waker | Pair as the smallest HID device that can wake the stick. | Proven with `MI RC` identity plus generic HID payload using System Wake Up. |
| Waker power toggle | Send a wake/power toggle input accepted by Android TV. | Available as a fallback Power action. |
| Explicit on/off instructions | Prefer separate "wake/on" and "sleep/off" actions instead of a blind toggle. | Explicit wake is proven via System Wake Up. System Sleep did not work; explicit off likely needs Android/CEC/API/IR instead of BLE HID. |

### With Stock Remote Connected

These are the acceptance tests for a usable setup.

| Goal | Desired result | Status |
| --- | --- | --- |
| Stock remote still paired | The physical Xiaomi remote remains paired and works normally. | Required. Xiaomi remote emulation conflicts with this if the TV only allows one Xiaomi RC. |
| Generic keyboard coexists | ESP pairs as a separate keyboard/media HID while the stock remote remains paired. | Lower priority because pure generic identity probably misses the wake whitelist. |
| Waker coexists | ESP wake action works while stock remote remains paired. | Proven enough for POC: the ESP paired as `MI RC` while the stock remote bond remained, after temporarily disabling the stock remote by removing its battery. |

## Current POC Behavior

- `wake` sends Generic Desktop System Wake Up and is the preferred explicit wake
  command.
- `sleep` sends Generic Desktop System Sleep, but this did not work on the Mi
  Stick during testing.
- `power` sends the generic Power bit and remains a toggle fallback.
- `home` and `back` send generic Android/Linux Home and Back inputs.
- The test YAML exposes an assumed-state switch whose ON action sends Wake and
  whose OFF action sends Sleep. The OFF side is experimental because Sleep did
  not work in the current POC.

## Next Experiments

1. Keep testing whether the ESP and stock remote remain stable across reboots,
   sleep cycles, and remote battery replacement.
2. Investigate explicit off through Android power-manager APIs, HDMI-CEC, ADB,
   or IR. BLE HID System Sleep was ignored by this Mi Stick.
3. If pairing becomes flaky, look for a way to trigger the setup guide's
   `allowMoreThanOneRC` path rather than relying on temporarily disabling the
   stock remote.
4. Only revisit a pure generic keyboard identity if wake-list admission can be
   solved another way.

## Reverse Engineering Notes

Using an upgrade image is a good way to avoid live-device permission limits.
If the upgrade package matches the stick's exact build, we can extract the same
APKs, framework files, vendor binaries, HIDL libraries, keylayout files, and
resources offline, then decompile or inspect them without Android shell
permissions getting in the way.

Useful targets to extract from a matching image:

- `system/priv-app/TvSettings/TvSettings.apk`
- `system/app/Bluetooth/Bluetooth.apk`
- `system/framework/`
- `vendor/bin/remotecfg`
- `vendor/bin/systemcontrol`
- `vendor/lib/` and `vendor/lib/hw/` Bluetooth, remote-control, and
  system-control libraries
- `vendor/usr/keylayout/`

The image must match the stick build closely. Otherwise, pairing-policy code,
resource IDs, and vendor service behavior may not line up with the device we are
testing.
