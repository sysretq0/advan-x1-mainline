#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Standalone MediaTek MT6789 MOLY Gen95 4G LTE bring-up helper for mainline wwan0.

Brings up SIM, configures MOLY APN + initial EPS attach bearer, activates the
default PDN via AT+EAPNACT, binds the carrier IPv4 address from +EIF onto
wwan{id % 100}, installs a default route, and configures systemd-resolved DNS.

Automatically yields (exits 0) if ModemManager.service is active unless --force
is passed, so it never collides with ModemManager on /dev/wwan0at0.
"""
import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import time

AT_DEV = "/dev/wwan0at0"
MODEM_SYSFS = "/sys/bus/platform/devices/d0cf000.modem"
DEFAULTS_FILE = "/etc/default/mtk-wwan"


def log(msg):
    print(f"[mtk-wwan] {msg}", flush=True)


def load_config():
    cfg = {"APN": "internet", "METRIC": "700", "MTU": "1400"}
    if os.path.isfile(DEFAULTS_FILE):
        with open(DEFAULTS_FILE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip().strip('"').strip("'")
    return cfg


def modemmanager_active():
    r = subprocess.run(
        ["systemctl", "is-active", "--quiet", "ModemManager.service"],
        check=False,
    )
    return r.returncode == 0


def is_final(buf):
    for tok in (b"\r\nOK\r\n", b"\r\nERROR\r\n", b"+CME ERROR:", b"+CMS ERROR:"):
        if tok in buf:
            return True
    return False


def wait_resp(fd, timeout=5.0, wait_final=True):
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while True:
        rem = deadline - time.monotonic()
        if rem <= 0:
            break
        r, _, _ = select.select([fd], [], [], min(rem, 0.25))
        if not r:
            if not wait_final and buf:
                break
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            break
        buf.extend(chunk)
        if wait_final and is_final(buf):
            break
    return buf.decode("utf-8", errors="replace")


def fmt(s):
    parts = [ln.strip() for ln in s.replace("\r", "\n").splitlines() if ln.strip()]
    return " | ".join(parts) if parts else "<no-response>"


def parse_eif(text):
    for ln in text.replace("\r", "\n").splitlines():
        m = re.search(
            r'\+EIF:\s*(\d+)\s*,\s*"?ifup"?\s*,\s*"?3"?\s*,\s*"?(\d+\.\d+\.\d+\.\d+)"?',
            ln,
        )
        if m:
            return int(m.group(1)), m.group(2)
    return None


def parse_dns(text):
    dns = []
    for ln in text.replace("\r", "\n").splitlines():
        if "+CGCONTRDP:" not in ln:
            continue
        fields = [f.strip().strip('"') for f in ln.split("+CGCONTRDP:", 1)[1].split(",")]
        for idx in (5, 6):
            if idx < len(fields) and re.fullmatch(r"\d+\.\d+\.\d+\.\d+", fields[idx]):
                if fields[idx] != "0.0.0.0" and fields[idx] not in dns:
                    dns.append(fields[idx])
    return dns


def at_cmd(fd, cmd, timeout=5.0, eif_box=None):
    os.write(fd, (cmd + "\r").encode("ascii"))
    resp = wait_resp(fd, timeout=timeout, wait_final=True)
    log(f"{cmd} => {fmt(resp)}")
    if eif_box is not None and eif_box[0] is None:
        hit = parse_eif(resp)
        if hit:
            eif_box[0] = hit
    return resp


def do_disconnect(iface):
    if os.path.exists(AT_DEV):
        fd = os.open(AT_DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            at_cmd(fd, "AT+EAPNACT=0,0", timeout=10.0)
        finally:
            os.close(fd)
    if os.path.exists(f"/sys/class/net/{iface}"):
        subprocess.run(["ip", "addr", "flush", "dev", iface], check=False)
        subprocess.run(["ip", "link", "set", iface, "down"], check=False)
        if shutil.which("resolvectl"):
            subprocess.run(["resolvectl", "revert", iface], check=False)
    log(f"disconnected {iface}")


def main():
    cfg = load_config()
    ap = argparse.ArgumentParser(description="MT6789 mainline wwan0 4G LTE connect helper")
    ap.add_argument("--apn", default=cfg["APN"], help="Carrier APN (default from /etc/default/mtk-wwan)")
    ap.add_argument("--mtu", type=int, default=int(cfg["MTU"]), help="Interface MTU (default 1400)")
    ap.add_argument("--metric", type=int, default=int(cfg["METRIC"]), help="Default route metric (default 700)")
    ap.add_argument("--force", action="store_true", help="Run even if ModemManager.service is active")
    ap.add_argument("--disconnect", action="store_true", help="Tear down PDN bearer and wwan0")
    args = ap.parse_args()

    if not args.force and modemmanager_active():
        log("ModemManager.service is active; yielding control of /dev/wwan0at0")
        return 0

    if args.disconnect:
        do_disconnect("wwan0")
        return 0

    for _ in range(200):
        if os.path.exists(AT_DEV) and os.path.exists("/sys/class/net/wwan0"):
            break
        time.sleep(0.1)

    if not os.path.exists(AT_DEV):
        log(f"ERROR: {AT_DEV} did not appear")
        return 1

    time.sleep(0.5)
    eif_box = [None]
    fd = os.open(AT_DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        wait_resp(fd, timeout=0.3, wait_final=False)
        at_cmd(fd, "AT", 2.0)
        at_cmd(fd, "ATE0", 2.0)
        at_cmd(fd, "AT+CMEE=2", 2.0)

        cfun = at_cmd(fd, "AT+CFUN?", 3.0)
        if "+CFUN: 0" in cfun:
            at_cmd(fd, "AT+CFUN=4", 10.0)

        for _ in range(15):
            r = at_cmd(fd, "AT+CPIN?", 2.0)
            if "READY" in r:
                break
            time.sleep(1.0)
            wait_resp(fd, timeout=0.2, wait_final=False)

        at_cmd(fd, "AT+EGTYPE=4", 5.0)
        at_cmd(fd, "AT+EAPNLOCK=1", 5.0)
        at_cmd(fd, 'AT+EAPNSET="",1,"",""', 5.0)
        at_cmd(
            fd,
            f'AT+EAPNSET="{args.apn}",2,"type=default,supl;protocol=IPV4V6;roaming_protocol=IPV4V6;'
            'authtype=0;carrier_enabled=1;max_conns=0;max_conns_time=0;wait_time=0;'
            'bearer_bitmask=0;inactive_timer=0"',
            5.0,
        )
        at_cmd(fd, "AT+EAPNLOCK=0", 5.0)
        at_cmd(fd, "AT+ECNCFG=1,,,1,1,1", 5.0)
        at_cmd(fd, f'AT+EIAAPN="{args.apn}",0,"IPV4V6","IPV4V6",0,"",""', 5.0)
        at_cmd(fd, "AT+EIAREG=1", 5.0)

        at_cmd(fd, "AT+EFUN=1", 10.0)
        at_cmd(fd, "AT+CFUN=1", 10.0)
        at_cmd(fd, "AT+CEREG=2", 2.0)

        for _ in range(20):
            urc = wait_resp(fd, timeout=0.2, wait_final=False)
            hit = parse_eif(urc)
            if hit and eif_box[0] is None:
                eif_box[0] = hit
            r = at_cmd(fd, "AT+CEREG?", 2.0, eif_box)
            if ",1" in r or ",5" in r:
                break
            time.sleep(1.5)

        at_cmd(fd, "AT+EDALLOW=1", 5.0, eif_box)
        at_cmd(fd, f'AT+EAPNACT=1,"{args.apn}","default",0', 15.0, eif_box)

        for _ in range(20):
            if eif_box[0] is not None:
                break
            urc = wait_resp(fd, timeout=1.0, wait_final=False)
            hit = parse_eif(urc)
            if hit:
                eif_box[0] = hit
                break

        if eif_box[0] is None:
            log("ERROR: no IPv4 +EIF ifup indication received")
            return 1

        eif_id, ip_addr = eif_box[0]
        ch = eif_id % 100
        iface = f"wwan{ch}"
        if not os.path.exists(f"/sys/class/net/{iface}"):
            subprocess.run(
                ["ip", "link", "add", iface, "parentdev", "d0cf000.modem", "type", "wwan", "linkid", str(ch)],
                check=False,
            )

        subprocess.run(["ip", "link", "set", iface, "mtu", str(args.mtu), "up"], check=True)
        subprocess.run(["ip", "addr", "replace", f"{ip_addr}/32", "dev", iface], check=True)
        subprocess.run(
            ["ip", "route", "replace", "default", "dev", iface, "metric", str(args.metric)],
            check=True,
        )

        at_cmd(fd, f'AT+EIF={eif_id},"ifst","up",3', 5.0)
        cg = at_cmd(fd, "AT+CGCONTRDP", 3.0)
        dns = parse_dns(cg)
        if dns and shutil.which("resolvectl"):
            subprocess.run(["resolvectl", "dns", iface, *dns], check=False)
            subprocess.run(["resolvectl", "domain", iface, "~."], check=False)

        log(f"CONNECTED: iface={iface} ip={ip_addr}/32 mtu={args.mtu} metric={args.metric} dns={dns}")
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
