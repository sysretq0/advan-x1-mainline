# mtk-wwan-connect

Lightweight standalone 4G LTE connect helper for the mainline `mtk_ccci` driver
(`/dev/wwan0at0` + `wwan0`).

- Configure carrier APN in `/etc/default/mtk-wwan` (defaults to `APN=internet`,
  `MTU=1400`, `METRIC=700`).
- Automatically yields if `ModemManager.service` is active (unless `--force` is
  passed), so it can coexist cleanly with the `modemmanager-mtk-ccci` plugin.
- Manual usage:
  ```sh
  sudo systemctl stop ModemManager
  sudo mtk-wwan-connect --apn internet
  sudo mtk-wwan-connect --disconnect
  ```
