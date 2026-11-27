#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""mtk-imsd: native Linux VoLTE & SMS-over-IMS daemon for Advan X1 (MT6789).

Talks to the MT6789 modem through ModemManager's mtk-ccci IMS bridge
(/run/mtk-ims-bridge.sock), brings up a dedicated mainline WWAN netdev
(wwan1 bound to the IMS PDN's DPMAIF net_if via `ip link add ... type wwan`),
authenticates with 3GPP IMS-AKA (USIM via AT+CSIM) + kernel IPsec (ip xfrm,
supporting hmac-sha-1-96 / hmac-md5-96 and null / aes-cbc), and handles:
  - VoLTE voice calls (SIP INVITE/183/PRACK/180/200/ACK/BYE/CANCEL + SDP
    AMR-WB / AMR-NB / PCMU / PCMA + mtk_volte_rtp PipeWire audio engine)
  - SMS over IP (SIP MESSAGE / RP-DATA / RP-ACK bridged into ModemManager)

Adapted and extended for mainline Linux WWAN & VoLTE voice from Zinwa Q25's
q25-imsd (GPL-2.0-or-later).

Usage:
  mtk-imsd run
  mtk-imsd status
  mtk-imsd dial <number>
  mtk-imsd answer
  mtk-imsd hangup
  mtk-imsd dtmf <digit>
  mtk-imsd send <number> <text>
"""
import base64
import ctypes
import hashlib
import ipaddress
import json
import os
import pwd
import queue
import random
import re
import select
import signal
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smspdu  # noqa: E402

BRIDGE = "/run/mtk-ims-bridge.sock"
CTL = "/run/mtk-imsd.sock"
STATE_DIR = "/var/lib/mtk-imsd"
LOG_PREFIX = "mtk-imsd"
RT_TABLE = 1403
IMS_IFNAME = "wwan1"
WWAN_PARENT = "d0cf000.modem"
WWAN0_IF_ID_SYSFS = "/sys/bus/platform/devices/d0cf000.modem/wwan0_if_id"
PDN_STATE = "/run/mtk-imsd.pdn"
XFRM_STATE = "/run/mtk-imsd.xfrm"
RTP_BIN = "/usr/local/sbin/mtk_volte_rtp"
SMS_LOG_MAX = 1 << 20


def log(*a):
    print(f"{time.strftime('%H:%M:%S')} {LOG_PREFIX}:", *a, flush=True)


def sh(*cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"{' '.join(cmd)}: {r.stderr.strip()}")
    return r.stdout


def ip_batch(lines, check=True):
    r = subprocess.run(
        ["ip", "-force", "-batch", "-"],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
    )
    if check and r.returncode:
        raise RuntimeError(
            f"ip -batch failed ({len(lines)} commands): "
            + re.sub(r"0x[0-9a-fA-F]{16,}", "<key>", r.stderr.strip())
        )


def desktop_user():
    try:
        for d in os.listdir("/run/user"):
            if d.isdigit() and int(d) >= 1000:
                return pwd.getpwuid(int(d))
    except (OSError, KeyError):
        pass
    try:
        return pwd.getpwuid(1000)
    except KeyError:
        return None


def notify_desktop(summary, body):
    u = desktop_user()
    if not u:
        return
    try:
        subprocess.run(
            [
                "runuser", "-u", u.pw_name, "--", "env",
                f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{u.pw_uid}/bus",
                "notify-send", "-a", "VoLTE", summary, body,
            ],
            check=False,
            timeout=10,
        )
    except Exception:
        pass


def has_lib(name):
    try:
        ctypes.CDLL(name)
        return True
    except OSError:
        return False


def mask_num(s):
    return re.sub(r"(\+?\d{3})\d{4,}(\d{3})", r"\1****\2", s)


def fmt_host(addr):
    return f"[{addr}]" if ":" in addr else addr


# ---------------------------------------------------------------- modem (AT)
class Modem:
    """AT + URC access through ModemManager's mtk-ccci IMS bridge."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(BRIDGE)
        self.wlock = threading.Lock()
        self.next_id = 0
        self.waiters = {}
        self.urc_handlers = []
        self.mo_handler = None
        self.mtres = queue.Queue()
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()

    def send_line(self, line):
        with self.wlock:
            self.sock.sendall(line.encode() + b"\n")

    def _reader(self):
        try:
            self._read_lines()
        except Exception as e:
            log("bridge reader error:", repr(e))
        self.alive = False
        log("ModemManager bridge closed; exiting for restart")
        os._exit(3)

    def _read_lines(self):
        f = self.sock.makefile("r", encoding="utf-8", errors="replace", newline="\n")
        for line in f:
            line = line.rstrip("\n")
            kind, _, rest = line.partition(" ")
            if kind == "ATRES":
                rid, _, rest = rest.partition(" ")
                status, _, text = rest.partition(" ")
                w = self.waiters.get(rid)
                if w:
                    w[1] = (status == "OK", text.replace("\\n", "\n"))
                    w[0].set()
            elif kind == "URC":
                for fn in self.urc_handlers:
                    try:
                        fn(rest)
                    except Exception as e:
                        log("URC handler error:", repr(e))
            elif kind == "MO" and self.mo_handler:
                rid, _, hexpdu = rest.partition(" ")
                try:
                    pdu = bytes.fromhex(hexpdu)
                except ValueError:
                    self.send_line(f"MORES {rid} ERR bad PDU")
                    continue
                self.mo_handler(rid, pdu)
            elif kind == "MTRES":
                self.mtres.put(rest)

    def at(self, cmd, timeout=90):
        self.next_id += 1
        rid = str(self.next_id)
        w = [threading.Event(), None]
        self.waiters[rid] = w
        self.send_line(f"AT {rid} {cmd}")
        if not w[0].wait(timeout + 5):
            self.waiters.pop(rid, None)
            raise RuntimeError(f"{cmd}: no answer from ModemManager")
        self.waiters.pop(rid, None)
        ok, text = w[1]
        if not ok:
            raise RuntimeError(f"{cmd}: {text}")
        return text

    def imsi(self):
        return re.search(r"\d{15}", self.at("AT+CIMI")).group(0)

    def imei(self):
        return re.search(r"\d{15}", self.at("AT+CGSN")).group(0)

    def smsc(self):
        try:
            m = re.search(r'\+CSCA:\s*"([^"]+)"', self.at("AT+CSCA?"))
            if m:
                num = m.group(1)
                return num if num.startswith("+") else "+" + num
        except Exception:
            pass
        return ""

    def cell_id_3gpp(self):
        """Best-effort E-UTRAN cell id for P-Access-Network-Info."""
        try:
            r = self.at("AT+CEREG?")
            m = re.search(r'\+CEREG:\s*\d+,\d+,"([0-9A-Fa-f]+)","([0-9A-Fa-f]+)"', r)
            if m:
                return m.group(1).zfill(4).upper() + m.group(2).zfill(7).upper()
        except Exception:
            pass
        return None

    def aka(self, rand, autn):
        apdu = "0088008122" + "10" + rand.hex().upper() + "10" + autn.hex().upper()
        resp = self._csim(apdu)
        if resp[-4:].startswith("61"):
            resp = self._csim("00C00000" + resp[-2:])
        sw, data = resp[-4:], bytes.fromhex(resp[:-4])
        if data[:1] == b"\xdb":
            i, out = 1, []
            for _ in range(3):
                n = data[i]
                out.append(data[i + 1:i + 1 + n])
                i += 1 + n
            return {"res": out[0], "ck": out[1], "ik": out[2]}
        if data[:1] == b"\xdc":
            return {"auts": data[2:2 + data[1]]}
        raise RuntimeError(f"USIM AUTHENTICATE failed: SW {sw}")

    def _csim(self, apdu):
        r = self.at(f'AT+CSIM={len(apdu)},"{apdu}"')
        return r.split('"')[1]


# ---------------------------------------------------------------- IMS PDN
def dec_ip(raw):
    raw = raw.strip().strip('"')
    if not raw:
        return None
    parts = raw.split(".")
    if len(parts) == 16 and all(p.isdigit() for p in parts):
        return socket.inet_ntop(socket.AF_INET6, bytes(int(x) for x in parts))
    if len(parts) == 4 and all(p.isdigit() for p in parts):
        return raw
    try:
        return str(ipaddress.ip_address(raw))
    except ValueError:
        return None


def is_ipv6(addr):
    return bool(addr and ":" in addr)


class Pdn:
    """Manages the dedicated IMS PDN on mainline wwan1 (DPMAIF net_if)."""

    def __init__(self, modem):
        self.m = modem
        self.ifname = IMS_IFNAME
        self.addr = None
        self.family = socket.AF_INET6
        self.pcscf = []
        self.eif = queue.Queue()
        self.eif_id = None
        self.on_lost = None
        modem.urc_handlers.append(self._on_urc)

    def _on_urc(self, line):
        m = re.search(
            r'\+EIF:\s*(\d+),\s*"?(ifup|ifdown)"?(?:,\s*(\d+))?(?:,\s*"([^"]*)")?',
            line,
        )
        if not m:
            return
        eid, ev = int(m.group(1)), m.group(2)
        atype = m.group(3)
        ip_str = m.group(4) or ""
        self.eif.put((eid, ev, atype, ip_str))
        if ev == "ifdown" and self.eif_id is not None and eid == self.eif_id:
            log(f"IMS bearer {self.ifname} (EIF {eid}) went down")
            self.invalidate()
            if self.on_lost:
                self.on_lost()

    def invalidate(self):
        self.addr = None
        self.eif_id = None
        try:
            os.unlink(PDN_STATE)
        except OSError:
            pass

    def contrdp(self):
        """Return (pcscf_list, local_addrs) for the active 'ims' PDN."""
        pcscf_v6, pcscf_v4 = [], []
        local_v6, local_v4 = [], []
        try:
            out = self.m.at("AT+CGCONTRDP")
        except Exception:
            return [], []
        for chunk in out.split("+CGCONTRDP:"):
            f = [x.strip().strip('"') for x in chunk.split(",")]
            if len(f) < 4:
                continue
            apn = f[2].lower()
            if apn != "ims" and not apn.startswith("ims."):
                continue
            # f[3] is local_addr[.subnet_mask]
            raw_loc = f[3].split(".")[0:16] if f[3].count(".") >= 15 else f[3].split(".")[0:4]
            loc = dec_ip(".".join(raw_loc)) if raw_loc else None
            if loc:
                (local_v6 if is_ipv6(loc) else local_v4).append(loc)
            # f[7], f[8] are P-CSCF primary & secondary
            for idx in (7, 8):
                if len(f) > idx and f[idx]:
                    ip = dec_ip(f[idx])
                    if ip:
                        (pcscf_v6 if is_ipv6(ip) else pcscf_v4).append(ip)
        # Prefer IPv6 if available, else IPv4
        if pcscf_v6:
            return pcscf_v6, local_v6
        return pcscf_v4, local_v4

    def _ensure_wwan1(self, net_if):
        """Create/rebind wwan1 on mainline mtk_ccci_dpmaif for net_if."""
        # Avoid colliding with wwan0 if wwan0 is idle on the same net_if
        try:
            cur_wwan0 = int(open(WWAN0_IF_ID_SYSFS).read().strip())
            if cur_wwan0 == net_if:
                st = sh("ip", "link", "show", "dev", "wwan0", check=False)
                if "state UP" not in st:
                    park = 20 if net_if != 20 else 19
                    with open(WWAN0_IF_ID_SYSFS, "w") as f:
                        f.write(str(park))
                    log(f"parked idle wwan0 on net_if {park} to free net_if {net_if} for {self.ifname}")
        except (OSError, ValueError):
            pass

        # Recreate wwan1 cleanly for this net_if
        sh("ip", "link", "del", self.ifname, check=False)
        sh(
            "ip", "link", "add", "dev", self.ifname,
            "parentdev", WWAN_PARENT, "type", "wwan", "linkid", str(net_if),
        )
        sh("ip", "link", "set", self.ifname, "mtu", "1400")
        sh("ip", "link", "set", self.ifname, "up")

    def up(self):
        pcscf, locals_ = self.contrdp()
        eif_ip = None
        if not pcscf:
            log("bringing up IMS PDN")
            self.m.at("AT+EIMSCFG=1,1,0,0,1,1")
            self.m.at("AT+EIMS=1")
            try:
                self.m.at("AT+CIREG=2")
            except Exception:
                pass
            while not self.eif.empty():
                self.eif.get_nowait()
            self.m.at('AT+EAPNACT=1,"ims","ims",0', timeout=90)
            eif = None
            ack_type = 3
            end = time.time() + 20
            while time.time() < end:
                try:
                    i, ev, typ, ip_str = self.eif.get(timeout=1)
                except queue.Empty:
                    continue
                if ev == "ifup":
                    eif = i
                    if typ in ("1", "2", "3"):
                        ack_type = int(typ)
                    if ip_str:
                        eif_ip = ip_str
                    break
            if eif is None:
                raise RuntimeError("no +EIF indication for IMS PDN")
            self.m.at(f'AT+EIF={eif},"ifst","up",{ack_type}')
            self.eif_id = eif
            with open(PDN_STATE, "w") as f:
                f.write(str(eif))
            pcscf, locals_ = self.contrdp()
        else:
            self.eif_id = self._load_saved_eif()

        if self.eif_id is None:
            raise RuntimeError("IMS PDN active but EIF id unknown; restart ModemManager")

        net_if = self.eif_id % 100
        self._ensure_wwan1(net_if)
        self.pcscf = pcscf
        if not self.pcscf:
            raise RuntimeError("IMS PDN up but +CGCONTRDP returned no P-CSCF")

        want_v6 = is_ipv6(self.pcscf[0])
        self.family = socket.AF_INET6 if want_v6 else socket.AF_INET

        if want_v6:
            # Allow SLAAC or configure static /64 from +CGCONTRDP / +EIF
            addr = None
            for _ in range(15):
                a = self.global_addrs()
                if a:
                    addr = a[0]
                    break
                time.sleep(1)
            if not addr:
                cand = next((x for x in locals_ if is_ipv6(x)), None) or (
                    eif_ip if is_ipv6(eif_ip) else None
                )
                if cand:
                    sh("ip", "-6", "addr", "replace", f"{cand}/64", "dev", self.ifname)
                    addr = cand
            if not addr:
                raise RuntimeError(f"no IPv6 address on {self.ifname}")
            self.addr = addr
            sh("ip", "-6", "route", "replace", "default", "dev", self.ifname, "table", str(RT_TABLE))
            sh("ip", "-6", "rule", "del", "priority", str(RT_TABLE), check=False)
            sh("ip", "-6", "rule", "add", "from", self.addr, "table", str(RT_TABLE), "priority", str(RT_TABLE))
        else:
            cand = next((x for x in locals_ if not is_ipv6(x)), None) or (
                eif_ip if (eif_ip and not is_ipv6(eif_ip)) else None
            )
            if not cand:
                raise RuntimeError(f"no IPv4 address on {self.ifname}")
            self.addr = cand
            sh("ip", "addr", "replace", f"{self.addr}/32", "dev", self.ifname)
            sh("ip", "route", "replace", "default", "dev", self.ifname, "table", str(RT_TABLE))
            sh("ip", "rule", "del", "priority", str(RT_TABLE), check=False)
            sh("ip", "rule", "add", "from", self.addr, "table", str(RT_TABLE), "priority", str(RT_TABLE))

        log(f"IMS PDN ready on {self.ifname} (net_if={net_if}) addr={self.addr} P-CSCF={self.pcscf}")

    def global_addrs(self):
        if self.family == socket.AF_INET6:
            out = sh("ip", "-6", "addr", "show", "dev", self.ifname, check=False)
            return re.findall(r"inet6 ([0-9a-f:]+)/\d+ scope global", out)
        out = sh("ip", "-4", "addr", "show", "dev", self.ifname, check=False)
        return re.findall(r"inet ([0-9.]+)/\d+", out)

    def _load_saved_eif(self):
        try:
            return int(open(PDN_STATE).read().strip())
        except (OSError, ValueError):
            return None


# ---------------------------------------------------------------- IPsec (XFRM)
class IPsec:
    """ESP transport SAs for 3GPP sec-agree (RFC 3329 / TS 33.203)."""

    def __init__(self):
        self.active = []
        self._cleanup_previous()

    @staticmethod
    def _del_cmds(handle):
        sas, pols = handle
        cmds = []
        for src, sp, dst, dp, d, _ in pols:
            for proto in ("udp", "tcp"):
                cmds.append(
                    f"xfrm policy del src {src} dst {dst} proto {proto} "
                    f"sport {sp} dport {dp} dir {d}"
                )
        for src, dst, spi in sas:
            cmds.append(f"xfrm state del src {src} dst {dst} proto esp spi {spi}")
        return cmds

    def _save(self):
        try:
            with open(XFRM_STATE + ".tmp", "w") as f:
                json.dump(self.active, f)
            os.replace(XFRM_STATE + ".tmp", XFRM_STATE)
        except OSError as e:
            log("cannot record xfrm state:", e)

    def _cleanup_previous(self):
        try:
            old = json.load(open(XFRM_STATE))
        except (OSError, ValueError):
            return
        cmds = [c for h in old for c in self._del_cmds(h)]
        if cmds:
            ip_batch(cmds, check=False)
        try:
            os.unlink(XFRM_STATE)
        except OSError:
            pass

    def setup(self, ue, pc, ue_c, ue_s, pc_c, pc_s, spi_uc, spi_us, spi_pc, spi_ps,
              ik, ck, alg="hmac-sha-1-96", ealg="null"):
        if alg == "hmac-md5-96":
            auth_spec = f"auth-trunc hmac(md5) 0x{ik.hex()} 96"
        else:
            auth_spec = f"auth-trunc hmac(sha1) 0x{(ik + b'\x00' * 4).hex()} 96"

        if ealg == "aes-cbc":
            enc_spec = f"enc cbc(aes) 0x{ck.hex()}"
        elif ealg == "des-ede3-cbc":
            enc_spec = f"enc cbc(des3_ede) 0x{(ck + ck[:8]).hex()}"
        else:
            enc_spec = 'enc cipher_null ""'

        sas = [
            (ue, pc, spi_ps),
            (ue, pc, spi_pc),
            (pc, ue, spi_us),
            (pc, ue, spi_uc),
        ]
        pols = [
            (ue, ue_c, pc, pc_s, "out", spi_ps),
            (ue, ue_s, pc, pc_c, "out", spi_pc),
            (pc, pc_c, ue, ue_s, "in", spi_us),
            (pc, pc_s, ue, ue_c, "in", spi_uc),
        ]
        handle = [[list(x) for x in sas], [list(x) for x in pols]]
        self.active.append(handle)
        self._save()
        cmds = [
            f"xfrm state add src {src} dst {dst} proto esp spi {spi} mode transport "
            f"{auth_spec} {enc_spec}"
            for src, dst, spi in sas
        ]
        for src, sp, dst, dp, d, spi in pols:
            for proto in ("udp", "tcp"):
                cmds.append(
                    f"xfrm policy add src {src} dst {dst} proto {proto} "
                    f"sport {sp} dport {dp} dir {d} "
                    f"tmpl src {src} dst {dst} proto esp spi {spi} mode transport"
                )
        try:
            ip_batch(cmds)
        except Exception:
            self.remove(handle)
            raise
        return handle

    def remove(self, handle):
        if handle in self.active:
            self.active.remove(handle)
        ip_batch(self._del_cmds(handle), check=False)
        self._save()

    def flush_old(self, keep=1):
        while len(self.active) > keep:
            self.remove(self.active[0])

    def flush_all(self):
        self.flush_old(keep=0)


# ---------------------------------------------------------------- SIP & SDP
def parse_sip(data):
    raw = data if isinstance(data, bytes) else data.encode()
    sep = raw.find(b"\r\n\r\n")
    head_bytes = raw[:sep] if sep >= 0 else raw
    body = raw[sep + 4:] if sep >= 0 else b""
    lines = head_bytes.decode("utf-8", errors="replace").split("\r\n")
    start = lines[0]
    hdrs = []
    for ln in lines[1:]:
        if ln[:1] in (" ", "\t") and hdrs:
            hdrs[-1] = (hdrs[-1][0], hdrs[-1][1] + " " + ln.strip())
        elif ":" in ln:
            k, v = ln.split(":", 1)
            hdrs.append((k.strip().lower(), v.strip()))
    short = {
        "v": "via", "f": "from", "t": "to", "i": "call-id",
        "m": "contact", "l": "content-length", "c": "content-type",
    }
    hdrs = [(short.get(k, k), v) for k, v in hdrs]
    msg = {"start": start, "hdrs": hdrs, "body": body}
    if start.startswith("SIP/2.0"):
        msg["code"] = int(start.split()[1])
    else:
        msg["method"] = start.split()[0]
    return msg


def h(msg, name):
    for k, v in msg["hdrs"]:
        if k == name:
            return v
    return None


def hall(msg, name):
    return [v for k, v in msg["hdrs"] if k == name]


def branch():
    return "z9hG4bK%016x" % random.getrandbits(64)


def tag():
    return "%08x" % random.getrandbits(32)


def supported_codecs():
    """Return ordered list of locally supported VoLTE audio codecs."""
    out = []
    if has_lib("libopencore-amrwb.so.0") and has_lib("libvo-amrwbenc.so.0"):
        out.append(("amr-wb", 97, 16000, 1))
        out.append(("amr-wb", 98, 16000, 0))
    if has_lib("libopencore-amrnb.so.0"):
        out.append(("amr-nb", 99, 8000, 1))
        out.append(("amr-nb", 100, 8000, 0))
    out.append(("pcmu", 0, 8000, 1))
    out.append(("pcma", 8, 8000, 1))
    return out


def build_sdp(local_ip, rtp_port, codecs=None, dtmf_pt=101):
    ip_ver = "IP6" if is_ipv6(local_ip) else "IP4"
    codecs = codecs or supported_codecs()
    pts = [str(c[1]) for c in codecs] + [str(dtmf_pt)]
    lines = [
        "v=0",
        f"o=mtk-imsd {int(time.time())} {int(time.time())} IN {ip_ver} {local_ip}",
        "s=VoLTE",
        f"c=IN {ip_ver} {local_ip}",
        "b=AS:49",
        "t=0 0",
        f"m=audio {rtp_port} RTP/AVP {' '.join(pts)}",
        "b=AS:49",
    ]
    for name, pt, rate, oa in codecs:
        if name == "amr-wb":
            lines.append(f"a=rtpmap:{pt} AMR-WB/{rate}")
            lines.append(f"a=fmtp:{pt} mode-change-capability=2;max-red=0;octet-align={oa}")
        elif name == "amr-nb":
            lines.append(f"a=rtpmap:{pt} AMR/{rate}")
            lines.append(f"a=fmtp:{pt} mode-change-capability=2;max-red=0;octet-align={oa}")
        elif name == "pcmu":
            lines.append(f"a=rtpmap:{pt} PCMU/8000")
        elif name == "pcma":
            lines.append(f"a=rtpmap:{pt} PCMA/8000")
    lines.append(f"a=rtpmap:{dtmf_pt} telephone-event/8000")
    lines.append(f"a=fmtp:{dtmf_pt} 0-15")
    lines.append("a=ptime:20")
    lines.append("a=maxptime:240")
    lines.append("a=sendrecv")
    return ("\r\n".join(lines) + "\r\n").encode()


def parse_remote_sdp(body):
    """Extract remote IP, audio port, and best mutually supported codec."""
    text = body.decode("utf-8", errors="replace") if isinstance(body, bytes) else body
    rip, rport = None, None
    in_audio = False
    offered_pts = []
    rtpmap = {}
    fmtp = {}
    for ln in text.splitlines():
        ln = ln.strip()
        if ln.startswith("c=IN "):
            parts = ln.split()
            if len(parts) >= 3 and (not in_audio or rip is None):
                rip = parts[2].split("/")[0]
        elif ln.startswith("m=audio "):
            in_audio = True
            parts = ln.split()
            rport = int(parts[1])
            offered_pts = [int(p) for p in parts[3:] if p.isdigit()]
        elif ln.startswith("m="):
            in_audio = False
        elif in_audio and ln.startswith("a=rtpmap:"):
            m = re.match(r"a=rtpmap:(\d+)\s+([^/\s]+)/(\d+)", ln)
            if m:
                rtpmap[int(m.group(1))] = (m.group(2).lower(), int(m.group(3)))
        elif in_audio and ln.startswith("a=fmtp:"):
            m = re.match(r"a=fmtp:(\d+)\s+(.*)", ln)
            if m:
                fmtp[int(m.group(1))] = m.group(2).lower()

    if not rip or not rport:
        return None

    dtmf_pt = 101
    for pt, (name, _) in rtpmap.items():
        if name == "telephone-event":
            dtmf_pt = pt
            break

    have_wb = has_lib("libopencore-amrwb.so.0") and has_lib("libvo-amrwbenc.so.0")
    have_nb = has_lib("libopencore-amrnb.so.0")

    def match_pt(want_name, want_oa=None):
        for pt in offered_pts:
            if pt == 0 and want_name == "pcmu":
                return ("pcmu", 0, 8000, 1)
            if pt == 8 and want_name == "pcma":
                return ("pcma", 8, 8000, 1)
            nm, rate = rtpmap.get(pt, ("", 0))
            if nm == want_name:
                oa = 1 if "octet-align=1" in fmtp.get(pt, "") else 0
                if want_oa is None or oa == want_oa:
                    cname = "amr-wb" if nm == "amr-wb" else ("amr-nb" if nm == "amr" else nm)
                    return (cname, pt, rate, oa)
        return None

    chosen = None
    if have_wb:
        chosen = match_pt("amr-wb", 1) or match_pt("amr-wb", 0)
    if not chosen and have_nb:
        chosen = match_pt("amr", 1) or match_pt("amr", 0)
    if not chosen:
        chosen = match_pt("pcmu") or match_pt("pcma")
    if not chosen:
        return None
    return {"ip": rip, "port": rport, "codec": chosen, "dtmf_pt": dtmf_pt}


# ---------------------------------------------------------------- IMS client
class CallSession:
    def __init__(self, call_id, remote_uri, direction):
        self.call_id = call_id
        self.remote_uri = remote_uri
        self.direction = direction  # "mo" | "mt"
        self.state = "dialing" if direction == "mo" else "ringing"
        self.local_tag = tag()
        self.remote_tag = ""
        self.remote_contact = remote_uri
        self.route_set = []
        self.cseq = 1
        self.local_rtp_port = random.randrange(20000, 40000, 2)
        self.invite_req = None
        self.invite_send = None
        self.invite_branch = None
        self.remote_sdp = None
        self.rtp_proc = None


class Ims:
    def __init__(self):
        self.m = Modem()
        self.pdn = Pdn(self.m)
        self.ipsec = IPsec()
        self.imsi = self.m.imsi()
        self.imei = self.m.imei()
        mcc, mnc = self.imsi[:3], self.imsi[3:5].zfill(3)
        self.domain = f"ims.mnc{mnc}.mcc{mcc}.3gppnetwork.org"
        self.impi = f"{self.imsi}@{self.domain}"
        self.impu = f"sip:{self.imsi}@{self.domain}"
        self.instance = f"urn:gsma:imei:{self.imei[:8]}-{self.imei[8:14]}-0"
        self.smsc = self.m.smsc()
        self.callid = "%016x@advanx1" % random.getrandbits(64)
        self.cseq = 0
        self.ports = [(5100, 5101), (5102, 5103)]
        self.pi = 0
        self.registered = False
        self.expires = 600000
        self.assoc = []
        self.service_route = []
        self.mr = random.randint(0, 255)
        self.pending = {}       # branch -> [Event, list_of_responses]
        self.sock_c = self.sock_s = None
        self.socks = []
        self.extra_socks = []
        self.wake = threading.Event()
        self.concat = {}
        self.work = queue.Queue()
        self.mo_wait = {}
        self.mr_lock = threading.Lock()
        self.seen = {}
        self.peers = {}
        self.tcp_slots = threading.BoundedSemaphore(8)
        self.last_reg = None
        self.call = None
        self.call_lock = threading.Lock()
        self.pcscf = None
        self.pc_s = 5060
        self.ue_c = 5100
        self.ue_s = 5101
        os.makedirs(STATE_DIR, mode=0o700, exist_ok=True)

    # -------- transport
    def _new_sockets(self, pc, ps):
        af = self.pdn.family
        c = socket.socket(af, socket.SOCK_DGRAM)
        c.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        c.bind((self.pdn.addr, pc))
        sv = socket.socket(af, socket.SOCK_DGRAM)
        sv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sv.bind((self.pdn.addr, ps))
        t = socket.socket(af, socket.SOCK_STREAM)
        t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        t.bind((self.pdn.addr, ps))
        t.listen(4)
        return [c, sv, t]

    def _close(self, socks):
        for x in socks or []:
            self.peers.pop(x, None)
            try:
                x.close()
            except OSError:
                pass

    def send_request(self, raw, dest, timeout=10, sock=None, on_provisional=None):
        raw_b = raw.encode() if isinstance(raw, str) else raw
        b = re.search(rb"branch=([^;\r\n]+)", raw_b).group(1).decode()
        ev = threading.Event()
        q = queue.Queue()
        self.pending[b] = (ev, q)
        (sock or self.sock_c).sendto(raw_b, dest)
        final = None
        end = time.time() + timeout
        while time.time() < end:
            try:
                r = q.get(timeout=max(0.05, end - time.time()))
            except queue.Empty:
                continue
            if r["code"] < 200:
                if on_provisional:
                    on_provisional(r)
                continue
            final = r
            break
        self.pending.pop(b, None)
        return final

    def _dispatch(self, data, send, requests_ok=True):
        if not data.strip():
            return
        try:
            msg = parse_sip(data)
        except Exception as e:
            log("unparsable SIP:", e)
            return
        if "code" in msg:
            b = re.search(r"branch=([^;\s,]+)", h(msg, "via") or "")
            w = self.pending.get(b.group(1)) if b else None
            if w:
                w[1].put(msg)
                w[0].set()
        elif not requests_ok:
            log(f"dropping {msg['method']} on unprotected port")
        else:
            try:
                self.on_request(msg, send)
            except Exception as e:
                log("request handling error:", repr(e))

    def _tcp_conn(self, conn):
        buf = b""
        conn.settimeout(600)
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                buf += chunk
                if len(buf) > 1 << 20:
                    return
                while b"\r\n\r\n" in buf:
                    head, _, rest = buf.partition(b"\r\n\r\n")
                    m = re.search(rb"(?im)^(?:content-length|l)\s*:\s*(\d+)", head)
                    n = int(m.group(1)) if m else 0
                    if len(rest) < n:
                        break
                    self._dispatch(head + b"\r\n\r\n" + rest[:n], conn.sendall)
                    buf = rest[n:]
        except OSError:
            pass
        finally:
            conn.close()
            self.tcp_slots.release()

    def _peer_ok(self, sock, addr):
        ok = (addr[0], addr[1]) in self.peers.get(sock, ())
        if not ok:
            log(f"dropping packet from unexpected peer {fmt_host(addr[0])}:{addr[1]}")
        return ok

    def rx_loop(self):
        while True:
            socks = [x for x in (self.socks or []) + (self.extra_socks or []) if x.fileno() >= 0]
            try:
                rd, _, _ = select.select(socks, [], [], 1.0)
            except (OSError, ValueError):
                time.sleep(0.2)
                continue
            for x in rd:
                try:
                    if x.type == socket.SOCK_STREAM:
                        conn, addr = x.accept()
                        if not self._peer_ok(x, addr) or not self.tcp_slots.acquire(blocking=False):
                            conn.close()
                            continue
                        threading.Thread(target=self._tcp_conn, args=(conn,), daemon=True).start()
                        continue
                    data, addr = x.recvfrom(65535)
                except OSError:
                    continue
                if not self._peer_ok(x, addr):
                    continue
                self._dispatch(
                    data,
                    lambda b, x=x, addr=addr: x.sendto(b, addr),
                    requests_ok=(addr[1] != 5060 or not self.ipsec.active),
                )

    def worker_loop(self):
        while True:
            fn, args = self.work.get()
            try:
                fn(*args)
            except Exception as e:
                log("worker error:", repr(e))

    # -------- registration
    def register(self):
        self.pdn.up()
        last = None
        for pcscf in self.pdn.pcscf:
            if not self.pdn.addr:
                raise RuntimeError("IMS bearer lost during registration")
            try:
                self._register_via(pcscf)
                return
            except Exception as e:
                last = e
                log(f"P-CSCF {pcscf}: {e!r}")
        raise last or RuntimeError("no P-CSCF")

    def _register_via(self, pcscf):
        ue_c, ue_s = self.ports[self.pi]
        new = self._new_sockets(ue_c, ue_s)
        self.extra_socks = new
        sock_c = new[0]
        self.peers[sock_c] = {(pcscf, 5060)}
        handle = None
        try:
            spi_uc = random.randint(0x10000, 0x7FFFFFFF)
            spi_us = random.randint(0x10000, 0x7FFFFFFF)
            # Advertise standard 3GPP TS 33.203 algorithms (null + aes-cbc, sha1 + md5)
            sec_client = ", ".join([
                f"ipsec-3gpp;alg=hmac-sha-1-96;ealg=null;spi-c={spi_uc};spi-s={spi_us};port-c={ue_c};port-s={ue_s}",
                f"ipsec-3gpp;alg=hmac-sha-1-96;ealg=aes-cbc;spi-c={spi_uc};spi-s={spi_us};port-c={ue_c};port-s={ue_s}",
                f"ipsec-3gpp;alg=hmac-md5-96;ealg=null;spi-c={spi_uc};spi-s={spi_us};port-c={ue_c};port-s={ue_s}",
                f"ipsec-3gpp;alg=hmac-md5-96;ealg=aes-cbc;spi-c={spi_uc};spi-s={spi_us};port-c={ue_c};port-s={ue_s}",
            ])
            uri = f"sip:{self.domain}"
            auth = None
            for _attempt in range(3):
                r = self.send_request(
                    self._register_msg(ue_c, ue_s, sec_client, auth=auth),
                    (pcscf, 5060),
                    sock=sock_c,
                )
                if not r or r["code"] != 401:
                    raise RuntimeError(f"initial REGISTER: {r and r['start']}")
                www = h(r, "www-authenticate")
                nonce = re.search(r'nonce="([^"]+)"', www).group(1)
                realm = re.search(r'realm="([^"]+)"', www).group(1)
                qop_m = re.search(r'qop="([^"]+)"', www)
                qop = "auth" if (qop_m and "auth" in qop_m.group(1).split(",")) else None
                raw = base64.b64decode(nonce)
                aka = self.m.aka(raw[:16], raw[16:32])
                if "auts" not in aka:
                    break
                log("AKA sequence out of sync: sending AUTS")
                auth = build_digest_auth(
                    self.impi, realm, nonce, b"", "REGISTER", uri, qop=qop,
                    auts=base64.b64encode(aka["auts"]).decode(),
                )
            else:
                raise RuntimeError("AKA resynchronisation did not converge")

            offers = [x.strip() for v in hall(r, "security-server") for x in v.split(",") if x.strip()]
            ss = ", ".join(offers) if offers else None
            if offers:
                sv = pick_sec_offer(offers)
                pc_c, pc_s = int(sv["port-c"]), int(sv["port-s"])
                for x in new:
                    self.peers[x] = {(pcscf, pc_c), (pcscf, pc_s)}
                handle = self.ipsec.setup(
                    self.pdn.addr, pcscf, ue_c, ue_s, pc_c, pc_s,
                    spi_uc, spi_us, int(sv["spi-c"]), int(sv["spi-s"]),
                    aka["ik"], aka["ck"],
                    alg=sv.get("alg", "hmac-sha-1-96"),
                    ealg=sv.get("ealg", "null"),
                )
            else:
                pc_s = 5060
                for x in new:
                    self.peers[x] = {(pcscf, 5060)}

            auth = build_digest_auth(self.impi, realm, nonce, aka["res"], "REGISTER", uri, qop=qop)
            r = self.send_request(
                self._register_msg(ue_c, ue_s, sec_client if offers else None, auth=auth, sec_verify=ss),
                (pcscf, pc_s),
                sock=sock_c,
            )
            if not r or r["code"] != 200:
                raise RuntimeError(f"protected REGISTER: {r and r['start']}")
        except Exception:
            if handle:
                self.ipsec.remove(handle)
            self.extra_socks = []
            self._close(new)
            raise

        old = self.socks
        self.pcscf, self.pc_s, self.ue_c, self.ue_s = pcscf, pc_s, ue_c, ue_s
        self.sock_c, self.sock_s = new[0], new[1]
        self.socks = new
        self.extra_socks = []
        self.pi ^= 1
        self._close(old)
        self.ipsec.flush_old(keep=1)
        self.last_reg = (pcscf, new[0], ue_c, ue_s, pc_s, sec_client, auth, ss)
        self.registered = True
        self.m.send_line("REG 1")
        self.assoc = [re.sub(r"[<>]", "", x.strip()) for v in hall(r, "p-associated-uri") for x in v.split(",")]
        self.service_route = [x.strip() for v in hall(r, "service-route") for x in v.split(",")]
        exp = None
        for v in hall(r, "contact"):
            m = re.search(r"expires=(\d+)", v)
            if m:
                exp = int(m.group(1))
        self.expires = exp or int(h(r, "expires") or 3600)
        log(f"REGISTERED via {pcscf} for {self.expires}s; identities {self._mask(self.assoc)}")

    def on_bearer_lost(self):
        self.registered = False
        self._teardown_call()
        try:
            self.m.send_line("REG 0")
        except OSError:
            pass
        self.wake.set()

    def deregister(self):
        if not self.registered or not self.last_reg:
            return
        pcscf, sock, ue_c, ue_s, pc_s, _sc, auth, ss = self.last_reg
        self.registered = False
        try:
            raw = self._register_msg(ue_c, ue_s, None, auth=auth, sec_verify=ss, expires=0)
            r = self.send_request(raw, (pcscf, pc_s), timeout=3, sock=sock)
            if r and r["code"] == 401:
                www = h(r, "www-authenticate")
                nonce = re.search(r'nonce="([^"]+)"', www).group(1)
                realm = re.search(r'realm="([^"]+)"', www).group(1)
                qop_m = re.search(r'qop="([^"]+)"', www)
                qop = "auth" if (qop_m and "auth" in qop_m.group(1).split(",")) else None
                nr = base64.b64decode(nonce)
                aka = self.m.aka(nr[:16], nr[16:32])
                auth = build_digest_auth(self.impi, realm, nonce, aka["res"], "REGISTER", f"sip:{self.domain}", qop=qop)
                raw = self._register_msg(ue_c, ue_s, None, auth=auth, sec_verify=ss, expires=0)
                r = self.send_request(raw, (pcscf, pc_s), timeout=3, sock=sock)
            log(f"deregister: {r['start'] if r else 'no answer'}")
        except Exception as e:
            log("deregister failed:", repr(e))

    def _pani_header(self):
        cid = self.m.cell_id_3gpp()
        if cid:
            return f"P-Access-Network-Info: 3GPP-E-UTRAN-FDD;utran-cell-id-3gpp={self.imsi[:5]}{cid}"
        return None

    def _register_msg(self, pc, ps, sec_client, auth, sec_verify=None, expires=600000):
        self.cseq += 1
        ha = fmt_host(self.pdn.addr)
        lines = [
            f"REGISTER sip:{self.domain} SIP/2.0",
            f"Via: SIP/2.0/UDP {ha}:{pc};branch={branch()};rport",
            "Max-Forwards: 70",
            f"From: <{self.impu}>;tag={tag()}",
            f"To: <{self.impu}>",
            f"Call-ID: {self.callid}",
            f"CSeq: {self.cseq} REGISTER",
            f'Contact: <sip:{self.imsi}@{ha}:{ps}>;+sip.instance="<{self.instance}>";'
            f'+g.3gpp.smsip;+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel";'
            f'audio;+g.3gpp.mid-call;+g.3gpp.srvcc-alerting'
            + (";expires=0" if expires == 0 else ""),
            f"Expires: {expires}",
            "Supported: path,gruu,sec-agree",
        ]
        if sec_client or sec_verify:
            lines += ["Require: sec-agree", "Proxy-Require: sec-agree"]
        if sec_client:
            lines.append(f"Security-Client: {sec_client}")
        if sec_verify:
            lines.append(f"Security-Verify: {sec_verify}")
        pani = self._pani_header()
        if pani:
            lines.append(pani)
        if auth:
            lines.append(f"Authorization: {auth}")
        else:
            lines.append(
                f'Authorization: Digest username="{self.impi}",realm="{self.domain}",'
                f'uri="sip:{self.domain}",nonce="",response=""'
            )
        lines += [
            "Allow: INVITE,ACK,CANCEL,BYE,UPDATE,PRACK,MESSAGE,OPTIONS,NOTIFY",
            "User-Agent: AdvanX1-Linux-IMS/0.2",
            "Content-Length: 0", "", "",
        ]
        return "\r\n".join(lines)

    # -------- VoLTE RTP engine management
    def _start_rtp(self, call, remote_sdp):
        if call.rtp_proc and call.rtp_proc.poll() is None:
            return
        cname, pt, _rate, oa = remote_sdp["codec"]
        u = desktop_user()
        uid = u.pw_uid if u else 1000
        cmd = [
            RTP_BIN,
            "--local-ip", self.pdn.addr,
            "--local-port", str(call.local_rtp_port),
            "--remote-ip", remote_sdp["ip"],
            "--remote-port", str(remote_sdp["port"]),
            "--codec", cname,
            "--pt", str(pt),
            "--octet-align", str(oa),
            "--dtmf-pt", str(remote_sdp["dtmf_pt"]),
            "--ifname", self.pdn.ifname,
            "--uid", str(uid),
        ]
        log(f"starting RTP {cname} pt={pt} oa={oa} {self.pdn.addr}:{call.local_rtp_port} <-> {remote_sdp['ip']}:{remote_sdp['port']}")
        call.rtp_proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)

    def _teardown_call(self):
        with self.call_lock:
            call = self.call
            self.call = None
        if call and call.rtp_proc:
            try:
                if call.rtp_proc.stdin:
                    call.rtp_proc.stdin.write(b"QUIT\n")
                    call.rtp_proc.stdin.flush()
            except OSError:
                pass
            try:
                call.rtp_proc.terminate()
                call.rtp_proc.wait(timeout=2)
            except Exception:
                try:
                    call.rtp_proc.kill()
                except Exception:
                    pass

    # -------- VoLTE SIP call control
    def _from_uri(self):
        return next((u for u in self.assoc if u.startswith("sip:")), self.impu)

    def _send_prack(self, call, prov_resp):
        rseq = h(prov_resp, "rseq")
        cseq_h = h(prov_resp, "cseq")
        if not rseq or not cseq_h:
            return
        ha = fmt_host(self.pdn.addr)
        call.cseq += 1
        to_h = h(prov_resp, "to") or f"<{call.remote_uri}>"
        from_h = h(prov_resp, "from") or f"<{self._from_uri()}>;tag={call.local_tag}"
        contact_h = h(prov_resp, "contact")
        req_uri = re.search(r"<([^>]+)>", contact_h).group(1) if (contact_h and "<" in contact_h) else call.remote_uri
        lines = [
            f"PRACK {req_uri} SIP/2.0",
            f"Via: SIP/2.0/UDP {ha}:{self.ue_c};branch={branch()};rport",
            "Max-Forwards: 70",
        ]
        lines += [f"Route: {r}" for r in (call.route_set or self.service_route)]
        lines += [
            f"From: {from_h}",
            f"To: {to_h}",
            f"Call-ID: {call.call_id}",
            f"CSeq: {call.cseq} PRACK",
            f"RAck: {rseq.strip()} {cseq_h.strip()}",
            f"P-Preferred-Identity: <{self._from_uri()}>",
            "Content-Length: 0", "", "",
        ]
        self.sock_c.sendto("\r\n".join(lines).encode(), (self.pcscf, self.pc_s))

    def _send_ack(self, call, resp):
        ha = fmt_host(self.pdn.addr)
        cseq_num = (h(resp, "cseq") or "1 INVITE").split()[0]
        to_h = h(resp, "to") or f"<{call.remote_uri}>"
        from_h = h(resp, "from") or f"<{self._from_uri()}>;tag={call.local_tag}"
        contact_h = h(resp, "contact")
        req_uri = re.search(r"<([^>]+)>", contact_h).group(1) if (contact_h and "<" in contact_h) else call.remote_uri
        call.remote_contact = req_uri
        lines = [
            f"ACK {req_uri} SIP/2.0",
            f"Via: SIP/2.0/UDP {ha}:{self.ue_c};branch={branch()};rport",
            "Max-Forwards: 70",
        ]
        lines += [f"Route: {r}" for r in (call.route_set or self.service_route)]
        lines += [
            f"From: {from_h}",
            f"To: {to_h}",
            f"Call-ID: {call.call_id}",
            f"CSeq: {cseq_num} ACK",
            "Content-Length: 0", "", "",
        ]
        self.sock_c.sendto("\r\n".join(lines).encode(), (self.pcscf, self.pc_s))

    def dial(self, number):
        if not self.registered:
            raise RuntimeError("IMS not registered")
        with self.call_lock:
            if self.call is not None:
                raise RuntimeError("call already in progress")
            target = f"tel:{number}" if number.startswith("+") else f"sip:{number}@{self.domain};user=phone"
            call = CallSession("%016x@advanx1" % random.getrandbits(64), target, "mo")
            self.call = call

        ha = fmt_host(self.pdn.addr)
        from_uri = self._from_uri()
        sdp = build_sdp(self.pdn.addr, call.local_rtp_port)
        call.invite_branch = branch()
        lines = [
            f"INVITE {target} SIP/2.0",
            f"Via: SIP/2.0/UDP {ha}:{self.ue_c};branch={call.invite_branch};rport",
            "Max-Forwards: 70",
        ]
        lines += [f"Route: {r}" for r in self.service_route]
        lines += [
            f"From: <{from_uri}>;tag={call.local_tag}",
            f"To: <{target}>",
            f"Call-ID: {call.call_id}",
            f"CSeq: {call.cseq} INVITE",
            f'Contact: <sip:{self.imsi}@{ha}:{self.ue_s}>;+sip.instance="<{self.instance}>";'
            f'+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel";audio',
            f"P-Preferred-Identity: <{from_uri}>",
            'Accept-Contact: *;+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel"',
            "Supported: 100rel,timer",
            "Allow: INVITE,ACK,CANCEL,BYE,UPDATE,PRACK,OPTIONS",
            "Content-Type: application/sdp",
            f"Content-Length: {len(sdp)}",
        ]
        pani = self._pani_header()
        if pani:
            lines.insert(-2, pani)
        raw = ("\r\n".join(lines) + "\r\n\r\n").encode() + sdp

        def run_mo():
            def on_prov(resp):
                log(f"MO call provisional: {resp['start']}")
                rr = hall(resp, "record-route")
                if rr:
                    call.route_set = list(reversed(rr))
                to_h = h(resp, "to") or ""
                m = re.search(r"tag=([^;\s]+)", to_h)
                if m:
                    call.remote_tag = m.group(1)
                if resp["code"] in (180, 183):
                    call.state = "ringing"
                if "100rel" in (h(resp, "require") or "").lower():
                    self._send_prack(call, resp)
                if resp["body"]:
                    rsdp = parse_remote_sdp(resp["body"])
                    if rsdp:
                        call.remote_sdp = rsdp
                        self._start_rtp(call, rsdp)

            resp = self.send_request(raw, (self.pcscf, self.pc_s), timeout=60, on_provisional=on_prov)
            if not resp or resp["code"] != 200:
                log(f"MO call failed: {resp['start'] if resp else 'timeout'}")
                self._teardown_call()
                return
            rr = hall(resp, "record-route")
            if rr:
                call.route_set = list(reversed(rr))
            to_h = h(resp, "to") or ""
            m = re.search(r"tag=([^;\s]+)", to_h)
            if m:
                call.remote_tag = m.group(1)
            self._send_ack(call, resp)
            call.state = "active"
            rsdp = parse_remote_sdp(resp["body"]) if resp["body"] else call.remote_sdp
            if rsdp:
                call.remote_sdp = rsdp
                self._start_rtp(call, rsdp)
            log(f"MO VoLTE call active with {mask_num(number)}")

        threading.Thread(target=run_mo, daemon=True).start()
        return {"call_id": call.call_id, "state": call.state}

    def answer_call(self):
        with self.call_lock:
            call = self.call
        if not call or call.direction != "mt" or call.state != "ringing":
            raise RuntimeError("no incoming ringing call")
        if not call.remote_sdp:
            raise RuntimeError("incoming INVITE had no compatible audio codec")
        ha = fmt_host(self.pdn.addr)
        sdp = build_sdp(
            self.pdn.addr, call.local_rtp_port,
            codecs=[call.remote_sdp["codec"]],
            dtmf_pt=call.remote_sdp["dtmf_pt"],
        )
        extra = [
            f'Contact: <sip:{self.imsi}@{ha}:{self.ue_s}>;+sip.instance="<{self.instance}>";'
            f'+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel";audio',
            "Content-Type: application/sdp",
        ]
        self.reply(call.invite_req, call.invite_send, 200, "OK", extra=extra,
                   body=sdp, to_tag=call.local_tag)
        call.state = "active"
        self._start_rtp(call, call.remote_sdp)
        log("MT VoLTE call answered")
        return {"call_id": call.call_id, "state": "active"}

    def hangup_call(self):
        with self.call_lock:
            call = self.call
        if not call:
            return {"state": "idle"}
        ha = fmt_host(self.pdn.addr)
        try:
            if call.direction == "mt" and call.state == "ringing" and call.invite_req:
                self.reply(call.invite_req, call.invite_send, 603, "Decline", to_tag=call.local_tag)
            elif call.direction == "mo" and call.state in ("dialing", "ringing"):
                lines = [
                    f"CANCEL {call.remote_uri} SIP/2.0",
                    f"Via: SIP/2.0/UDP {ha}:{self.ue_c};branch={call.invite_branch};rport",
                    "Max-Forwards: 70",
                ]
                lines += [f"Route: {r}" for r in self.service_route]
                lines += [
                    f"From: <{self._from_uri()}>;tag={call.local_tag}",
                    f"To: <{call.remote_uri}>",
                    f"Call-ID: {call.call_id}",
                    f"CSeq: 1 CANCEL",
                    "Content-Length: 0", "", "",
                ]
                self.sock_c.sendto("\r\n".join(lines).encode(), (self.pcscf, self.pc_s))
            else:
                call.cseq += 1
                from_h = f"<{self._from_uri()}>;tag={call.local_tag}"
                to_h = f"<{call.remote_uri}>" + (f";tag={call.remote_tag}" if call.remote_tag else "")
                lines = [
                    f"BYE {call.remote_contact} SIP/2.0",
                    f"Via: SIP/2.0/UDP {ha}:{self.ue_c};branch={branch()};rport",
                    "Max-Forwards: 70",
                ]
                lines += [f"Route: {r}" for r in (call.route_set or self.service_route)]
                lines += [
                    f"From: {from_h}",
                    f"To: {to_h}",
                    f"Call-ID: {call.call_id}",
                    f"CSeq: {call.cseq} BYE",
                    "Content-Length: 0", "", "",
                ]
                self.sock_c.sendto("\r\n".join(lines).encode(), (self.pcscf, self.pc_s))
        finally:
            self._teardown_call()
        log("VoLTE call hung up")
        return {"state": "idle"}

    def send_dtmf(self, digit):
        with self.call_lock:
            call = self.call
        if not call or not call.rtp_proc or not call.rtp_proc.stdin:
            raise RuntimeError("no active VoLTE call")
        call.rtp_proc.stdin.write(f"DTMF {digit[0]}\n".encode())
        call.rtp_proc.stdin.flush()
        return {"ok": True}

    # -------- incoming requests
    def reply(self, req, send, code, reason, extra=(), body=b"", to_tag=None):
        lines = [f"SIP/2.0 {code} {reason}"]
        for k in ("via", "from", "to", "call-id", "cseq"):
            for v in hall(req, k):
                if k == "to" and "tag=" not in v:
                    v += f";tag={to_tag or tag()}"
                lines.append(f"{k.title() if k != 'call-id' else 'Call-ID'}: {v}")
        lines += list(extra) + [f"Content-Length: {len(body)}", "", ""]
        send("\r\n".join(lines).encode() + body)

    def on_request(self, req, send):
        meth = req["method"]
        key = (h(req, "call-id"), h(req, "cseq"))
        dup = key in self.seen
        now = time.time()
        self.seen = {k: t for k, t in self.seen.items() if now - t < 300}
        self.seen[key] = now
        log(f"<- {meth}{' (dup)' if dup else ''} {len(req['body'])} B")

        if meth == "MESSAGE":
            self.reply(req, send, 200, "OK")
            if dup:
                return
            ctype = (h(req, "content-type") or "").lower()
            if "vnd.3gpp.sms" in ctype:
                body = req["body"]
                if body and body[0] & 0x07 in (smspdu.RP_ACK_N_MS, smspdu.RP_ERROR_N_MS):
                    self.on_sms_rp(req)
                else:
                    self.work.put((self.on_sms_rp, (req,)))
        elif meth == "INVITE":
            if dup:
                return
            self.reply(req, send, 100, "Trying")
            cid = h(req, "call-id")
            with self.call_lock:
                if self.call is not None and self.call.call_id != cid:
                    self.reply(req, send, 486, "Busy Here")
                    return
            rsdp = parse_remote_sdp(req["body"]) if req["body"] else None
            if not rsdp:
                self.reply(req, send, 488, "Not Acceptable Here")
                return
            pai = h(req, "p-asserted-identity") or h(req, "from") or "unknown"
            m = re.search(r"<([^>]+)>", pai)
            caller = m.group(1) if m else pai
            call = CallSession(cid, caller, "mt")
            call.invite_req = req
            call.invite_send = send
            call.remote_sdp = rsdp
            from_h = h(req, "from") or ""
            tm = re.search(r"tag=([^;\s]+)", from_h)
            if tm:
                call.remote_tag = tm.group(1)
            ch = h(req, "contact")
            if ch and "<" in ch:
                call.remote_contact = re.search(r"<([^>]+)>", ch).group(1)
            rr = hall(req, "record-route")
            if rr:
                call.route_set = rr
            with self.call_lock:
                self.call = call
            ha = fmt_host(self.pdn.addr)
            self.reply(
                req, send, 180, "Ringing",
                extra=[f'Contact: <sip:{self.imsi}@{ha}:{self.ue_s}>;+sip.instance="<{self.instance}>";audio'],
                to_tag=call.local_tag,
            )
            log(f"incoming VoLTE call from {mask_num(caller)}")
            notify_desktop("Incoming VoLTE Call", f"From {caller}\nRun: mtk-imsd answer")
        elif meth == "ACK":
            pass
        elif meth == "BYE":
            self.reply(req, send, 200, "OK")
            cid = h(req, "call-id")
            if self.call and self.call.call_id == cid:
                log("remote party hung up (BYE)")
                self._teardown_call()
        elif meth == "CANCEL":
            self.reply(req, send, 200, "OK")
            cid = h(req, "call-id")
            if self.call and self.call.call_id == cid:
                if self.call.invite_req:
                    self.reply(self.call.invite_req, self.call.invite_send, 487,
                               "Request Terminated", to_tag=self.call.local_tag)
                self._teardown_call()
        elif meth in ("OPTIONS", "NOTIFY", "PRACK", "UPDATE"):
            self.reply(req, send, 200, "OK")
        else:
            self.reply(req, send, 405, "Method Not Allowed")

    # -------- SMS over IMS
    def on_sms_rp(self, req):
        rp = smspdu.decode_rp(req["body"])
        if rp["type"] == smspdu.RP_DATA_N_MS:
            handed = self.hand_to_mm(rp["oa"], rp["tpdu"])
            if not handed:
                self.deliver(smspdu.decode_deliver(rp["tpdu"]))
            self._send_rp(
                smspdu.rp_ack_ms_n(rp["mr"]),
                in_reply_to=h(req, "call-id"),
                to=re.search(r"<([^>]+)>", h(req, "from")).group(1),
                wait=False,
            )
        elif rp["type"] in (smspdu.RP_ACK_N_MS, smspdu.RP_ERROR_N_MS):
            ok = rp["type"] == smspdu.RP_ACK_N_MS
            w = self.mo_wait.get(rp["mr"])
            if w:
                w[1] = (ok, rp.get("cause"))
                w[0].set()

    def hand_to_mm(self, smsc, tpdu):
        while not self.m.mtres.empty():
            self.m.mtres.get_nowait()
        self.m.send_line(f"MT {smsc or '-'} {tpdu.hex()}")
        try:
            res = self.m.mtres.get(timeout=10)
        except queue.Empty:
            res = "ERR timeout"
        return res.startswith("OK")

    def deliver(self, d):
        if d["concat"]:
            ref, total, seq = d["concat"]
            key = (d["from"], ref)
            now = time.time()
            self.concat = {k: v for k, v in self.concat.items() if now - v["t"] < 86400}
            ent = self.concat.setdefault(key, {"t": now, "parts": {}})
            ent["parts"][seq] = d["text"]
            if len(ent["parts"]) < total:
                return
            d = dict(d, text="".join(ent["parts"][i] for i in sorted(ent["parts"])))
            del self.concat[key]
        rec = {"from": d["from"], "time": d["time"], "text": d["text"], "received": time.time()}
        path = os.path.join(STATE_DIR, "sms.jsonl")
        try:
            if os.path.getsize(path) > SMS_LOG_MAX:
                os.replace(path, path + ".1")
        except OSError:
            pass
        fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
        with os.fdopen(fd, "a") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        notify_desktop(f"SMS from {d['from']}", d["text"])

    def _send_rp(self, body, to, in_reply_to=None, wait=True):
        with self.mr_lock:
            self.cseq += 1
        ha = fmt_host(self.pdn.addr)
        from_uri = self._from_uri()
        lines = [
            f"MESSAGE {to} SIP/2.0",
            f"Via: SIP/2.0/UDP {ha}:{self.ue_c};branch={branch()};rport",
            "Max-Forwards: 70",
        ]
        lines += [f"Route: {r}" for r in self.service_route]
        lines += [
            f"From: <{from_uri}>;tag={tag()}",
            f"To: <{to}>",
            f"Call-ID: {'%016x' % random.getrandbits(64)}@advanx1",
            f"CSeq: {self.cseq} MESSAGE",
            f"P-Preferred-Identity: <{from_uri}>",
            "Content-Type: application/vnd.3gpp.sms",
            "Request-Disposition: no-fork",
            "Accept-Contact: *;+g.3gpp.smsip",
            "Allow: MESSAGE",
        ]
        if in_reply_to:
            lines.append(f"In-Reply-To: {in_reply_to}")
        lines += [f"Content-Length: {len(body)}", "", ""]
        raw = "\r\n".join(lines).encode() + body
        if not wait:
            self.sock_c.sendto(raw, (self.pcscf, self.pc_s))
            return None
        return self.send_request(raw, (self.pcscf, self.pc_s), timeout=15)

    def send_tpdu(self, tpdu):
        if not self.registered:
            return False, "not registered"
        with self.mr_lock:
            self.mr = (self.mr + 1) & 0xFF
            mr = self.mr
        w = [threading.Event(), None]
        self.mo_wait[mr] = w
        try:
            sc = f"sip:{self.smsc}@{self.domain};user=phone"
            r = self._send_rp(smspdu.rp_data_ms_n(mr, self.smsc, tpdu), to=sc)
            if not r or r["code"] >= 300:
                return False, r["start"] if r else "no SIP answer"
            if not w[0].wait(30):
                return False, "no RP-ACK from SMSC"
            ok, cause = w[1]
            return ok, "" if ok else f"RP cause {cause}"
        finally:
            self.mo_wait.pop(mr, None)

    def on_mo(self, rid, tpdu):
        def job():
            try:
                ok, why = self.send_tpdu(tpdu)
            except Exception as e:
                ok, why = False, repr(e)
            self.m.send_line(f"MORES {rid} OK" if ok else f"MORES {rid} ERR {why}")
        threading.Thread(target=job, daemon=True).start()

    def send_sms(self, number, text):
        if not self.registered:
            raise RuntimeError("not registered")
        results = []
        for tpdu in smspdu.encode_submit(number, text):
            ok, why = self.send_tpdu(tpdu)
            results.append("RP-ACK" if ok else why)
        return results

    # -------- control socket
    def ctl_loop(self):
        if os.path.exists(CTL):
            os.unlink(CTL)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(CTL)
        os.chmod(CTL, 0o600)
        srv.listen(4)
        while True:
            c, _ = srv.accept()
            try:
                req = json.loads(c.recv(65536).decode())
                cmd = req.get("cmd")
                if cmd == "send":
                    out = {"ok": True, "result": self.send_sms(req["to"], req["text"])}
                elif cmd == "dial":
                    out = {"ok": True, **self.dial(req["to"])}
                elif cmd == "answer":
                    out = {"ok": True, **self.answer_call()}
                elif cmd == "hangup":
                    out = {"ok": True, **self.hangup_call()}
                elif cmd == "dtmf":
                    out = self.send_dtmf(req["digit"])
                else:
                    call_info = None
                    with self.call_lock:
                        if self.call:
                            call_info = {
                                "call_id": self.call.call_id,
                                "direction": self.call.direction,
                                "state": self.call.state,
                                "remote": self.call.remote_uri,
                            }
                    out = {
                        "ok": True,
                        "registered": self.registered,
                        "pdn": self.pdn.ifname,
                        "addr": self.pdn.addr,
                        "pcscf": self.pdn.pcscf,
                        "expires": self.expires,
                        "codecs": [c[0] for c in supported_codecs()],
                        "call": call_info,
                    }
            except Exception as e:
                out = {"ok": False, "error": repr(e)}
            c.sendall(json.dumps(out).encode())
            c.close()

    def _mask(self, xs):
        return [mask_num(x.replace(self.imsi, self.imsi[:5] + "********" + self.imsi[-2:])) for x in xs]

    def run(self):
        def stop(signum, _frame):
            log(f"signal {signum}: shutting down")
            self._teardown_call()
            self.deregister()
            self.ipsec.flush_all()
            os._exit(0)

        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        threading.Thread(target=self.ctl_loop, daemon=True).start()
        threading.Thread(target=self.rx_loop, daemon=True).start()
        threading.Thread(target=self.worker_loop, daemon=True).start()
        self.m.mo_handler = self.on_mo
        self.pdn.on_lost = self.on_bearer_lost
        self.m.send_line("REG 0")
        rereg = int(os.environ.get("MTK_IMSD_REREG", "0"))
        backoff = 10
        while True:
            try:
                self.register()
                backoff = 10
                deadline = time.time() + (rereg or max(60, self.expires // 2))
                while time.time() < deadline:
                    if self.wake.wait(min(60, max(1, deadline - time.time()))):
                        break
                    if self.pdn.addr and self.pdn.addr not in self.pdn.global_addrs():
                        log("IMS address changed; re-registering")
                        self.pdn.addr = None
                        break
                self.wake.clear()
                log("re-registering")
            except Exception as e:
                self.registered = False
                try:
                    self.m.send_line("REG 0")
                except OSError:
                    pass
                log("registration failed:", repr(e), f"; retry in {backoff}s")
                self.wake.wait(backoff)
                self.wake.clear()
                backoff = min(backoff * 2, 600)


def pick_sec_offer(offers):
    good = []
    for o in offers:
        p = dict(re.findall(r"([a-z-]+)=([^;,\s]+)", o.lower()))
        if not o.lower().startswith("ipsec-3gpp"):
            continue
        if p.get("alg") not in ("hmac-sha-1-96", "hmac-md5-96"):
            continue
        if p.get("ealg", "null") not in ("null", "aes-cbc", "des-ede3-cbc"):
            continue
        if not all(k in p for k in ("spi-c", "spi-s", "port-c", "port-s")):
            continue
        good.append((float(p.get("q", "0.1")), p))
    if not good:
        raise RuntimeError(f"no usable Security-Server offer in {offers}")
    return max(good, key=lambda x: x[0])[1]


def build_digest_auth(username, realm, nonce, res, method, uri, qop=None, auts=None):
    ha1 = hashlib.md5(f"{username}:{realm}:".encode() + res).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    if qop == "auth":
        cnonce = "%08x" % random.getrandbits(32)
        nc = "00000001"
        resp = hashlib.md5(f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}".encode()).hexdigest()
        s = (
            f'Digest username="{username}",realm="{realm}",nonce="{nonce}",'
            f'uri="{uri}",qop=auth,nc={nc},cnonce="{cnonce}",'
            f'response="{resp}",algorithm=AKAv1-MD5'
        )
    else:
        resp = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
        s = (
            f'Digest username="{username}",realm="{realm}",nonce="{nonce}",'
            f'uri="{uri}",response="{resp}",algorithm=AKAv1-MD5'
        )
    if auts:
        s += f',auts="{auts}"'
    return s


def client(req):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(CTL)
    c.sendall(json.dumps(req).encode())
    print(c.recv(65536).decode())


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    if cmd == "send" and len(sys.argv) >= 4:
        client({"cmd": "send", "to": sys.argv[2], "text": " ".join(sys.argv[3:])})
    elif cmd == "dial" and len(sys.argv) >= 3:
        client({"cmd": "dial", "to": sys.argv[2]})
    elif cmd == "answer":
        client({"cmd": "answer"})
    elif cmd == "hangup":
        client({"cmd": "hangup"})
    elif cmd == "dtmf" and len(sys.argv) >= 3:
        client({"cmd": "dtmf", "digit": sys.argv[2]})
    elif cmd == "status":
        client({"cmd": "status"})
    elif cmd == "run":
        Ims().run()
    else:
        print("Usage: mtk-imsd {run|status|dial <num>|answer|hangup|dtmf <d>|send <num> <text>}", file=sys.stderr)
        sys.exit(2)
