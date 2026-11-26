# ModemManager MTK CCCI Plugin (Mainline WWAN)

Native ModemManager plugin for the mainline Linux `mtk_ccci` driver on
MediaTek MT6789 (Helio G99) using standard Linux WWAN ports (`/dev/wwan0at0`
under `SUBSYSTEM=="wwan"` and `wwan0` under `SUBSYSTEM=="net"`).

Built in CI against the current target Debian `sid`/`forky` `modemmanager`
source package via `scripts/build-userspace-tools.sh` and packaged in
`advan-x1-support_<ver>_arm64.deb`.

## Installed Paths
- `/usr/lib/aarch64-linux-gnu/ModemManager/libmm-plugin-mtk-ccci.so`
- `/etc/udev/rules.d/77-mm-mtk-ccci.rules`
- `/etc/systemd/system/ModemManager.service.d/10-mtk-ccci.conf`

## NetworkManager Mobile Profile
Configure both the bearer APN and initial EPS attach APN explicitly, and keep
`gsm.auto-config no` (avoids a NetworkManager 1.58 `libnm-wwan.so` race when
combined with `initial-eps-bearer-configure`). Replace `<apn>` with your
carrier's APN:
```sh
nmcli connection add type gsm ifname '*' con-name mobile \
    gsm.apn <apn> gsm.auto-config no \
    gsm.initial-eps-bearer-configure yes gsm.initial-eps-bearer-apn <apn> \
    connection.autoconnect yes
nmcli connection up mobile
```
