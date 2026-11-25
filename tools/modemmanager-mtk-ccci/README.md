# ModemManager MTK CCCI Plugin (Mainline WWAN)

Native ModemManager 1.24.2 plugin for the mainline Linux `mtk_ccci` driver on
MediaTek MT6789 (Helio G99) using standard Linux WWAN ports (`/dev/wwan0at0`
under `SUBSYSTEM=="wwan"` and `wwan0` under `SUBSYSTEM=="net"`).

## Installed Paths
- `/usr/lib/aarch64-linux-gnu/ModemManager/libmm-plugin-mtk-ccci.so`
- `/etc/udev/rules.d/77-mm-mtk-ccci.rules`
- `/etc/systemd/system/ModemManager.service.d/mtk-ccci.conf`

## NetworkManager Mobile Profile
Configure both the bearer APN and initial EPS attach APN once (replace
`internet` with your carrier's APN):
```sh
nmcli connection add type gsm ifname '*' con-name mobile \
    gsm.apn internet \
    gsm.initial-eps-bearer-configure yes gsm.initial-eps-bearer-apn internet \
    ipv6.method disabled connection.autoconnect yes
nmcli connection up mobile
```
