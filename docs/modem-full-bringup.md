# Advan X1 (MT6789) Full Modem Architecture (Vendor-Grounded)

This document records the stock Android vendor modem behavior verified directly
from Advan X1's live vendor stack (`libmtk-ril.so`, `audio.primary.mediatek.so`,
`/vendor/etc/audio_device.xml`, `volte_imsm_93`, `volte_imcb`, and stock
`logcat -b radio`) and how each stage maps onto our mainline Linux kernel +
ModemManager + ASoC stack in strict bring-up order: **Internet -> Call -> SMS -> VoLTE**.

---

## Stage 1: Cellular Internet (Data Plane + Suspend/Resume)

### Stock Behavior (`libmtk-ril.so` `NetAgentService` + `RmcDcCommon`)
- APN sync: `AT+EAPNLOCK=1` -> `AT+EAPNSET` (bare clear) -> `AT+EAPNSET="<apn>",1,"<u>","<p>"` ->
  `AT+EAPNSET="<apn>",2,"type=default,supl;protocol=<IP|IPV6|IPV4V6>;..."` -> `AT+EAPNLOCK=0` ->
  `AT+EIAAPN="<apn>",0,"<proto>","<proto>",<auth>,"<u>","<p>"` -> `AT+EIAREG=1`.
- PDN activation: `AT+EDALLOW=1` -> `AT+EAPNACT=1,"<apn>","default",0`.
- Interface up URC: `+EIF: <id>,"ifup",<addrType>,"<ip>"`, where
  `NetAgentService::addrTypeToString` defines:
  - `1` = `IPV4`
  - `2` = `IPV6`
  - `3` = `IPV4V6`
- Interface ACK: `AT+EIF=<id>,"ifst","up",<addrType>`.
- DNS / IPv6 lookup: `AT+CGCONTRDP` (returns separate lines for IPv4 and IPv6
  when dual-stack; IPv6 global `/64` or link-local SLAAC).
- PDP status poll: `AT+CGACT?` (`+CGACT: <cid>,<state>`).

### Mainline Implementation
- **Kernel (`mtk_ccci_dpmaif.c`):** DPMAIF v1 DL + SMEM UL (`wwan0`, supports
  IPv4 `ETH_P_IP` and IPv6 `ETH_P_IPV6`), plus defensive `mtk_ccci_dpmaif_suspend()`
  / `mtk_ccci_dpmaif_resume()` saving `DPMAIF_PD_AP_DL_L2TIMR0` and restoring
  interrupt masks and HW UL rings if DPMAIF PD loses power during deep sleep.
- **ModemManager (`libmm-plugin-mtk-ccci.so`):**
  - Full IPv4 (`1`) / IPv6 (`2`) / IPv4v6 (`3`) support in `+EIF:` parsing,
    `+EIF=<id>,"ifst","up",<type>` ACK, `+CGCONTRDP` IPv4/IPv6 DNS parsing,
    and `load_supported_ip_families()`.
  - Implements `load_connection_status` & `reload_connection_status` via
    `AT+CGACT?` so suspend/resume cleanly checks active PDNs.
- **NetworkManager profile fix:** NetworkManager 1.58.1 has an upstream race in
  `libnm-wwan.so` when a GSM connection has `auto-config=true` (no explicit `apn=`)
  combined with `initial-eps-bearer-configure=true`, firing two concurrent
  `set_initial_eps_bearer_settings()` calls that prematurely advance the state
  machine past `Simple.Connect()` with `invalid bearer IP configuration`. Always
  pin explicit `apn=<carrier-apn>` and `auto-config=false` in the NM GSM profile.

---

## Stage 2: CS Voice Call (AT Control + PCM2 Crossbar + `/dev/ccci_aud`)

### Stock Behavior (`libmtk-ril.so` + `audio.primary.mediatek.so` + `audio_device.xml`)
1. **AT initialization (`radio.log`):**
   - `AT+ECFGSET="disable_cs_call","0"`, `AT+CLIP=1`, `AT+COLP=1`,
     `AT+CSSN=1,1`, `AT+CMOD=0`, `ATS0=0`.
   - Keep `+EAIC` at default `0` so standard 3GPP `RING` / `+CLCC` / `ATD<num>;` /
     `ATA` / `ATH` / `+CHLD` work natively with ModemManager's `MMIfaceModemVoice`
     and GNOME Calls.
2. **Hardware AFE Voice Crossbar (`PCM 2` <-> `ADDA`):**
   - `PCM2_INTF_CON` (`0x053c`): `PCM_MODE_B`, `32 BCK`, `AFIFO`, `DUAL_MIC_ON_TX`,
     rate 8/16/32/48 kHz, `PCM2_EN` bit 0.
   - Downlink: `MD1_TO_AFE` -> `PCM 2 Capture` -> `ADDA_DL_CH1` (`AFE_CONN3`,
     `PCM_2_CAP_CH1 Switch`) & `ADDA_DL_CH2` (`AFE_CONN4`, `PCM_2_CAP_CH1 Switch`) ->
     `ADDA Playback` -> MT6366 Earpiece / Headphones / Speaker (`Lineout Volume <= 8`).
   - Uplink: MT6366 Mic / Headset Mic -> `ADDA Capture` -> `ADDA_UL_Mux` ->
     `PCM_2_PB_CH1` (`AFE_CONN17`) / `PCM_2_PB_CH2` (`AFE_CONN18`) ->
     `PCM 2 Playback` -> `AFE_TO_MD1`.
   - Driven hostlessly (zero CPU DMA) via `Hostless_Speech` (`hw:mt6789mt6366,5`)
     and UCM2 `VoiceCall.conf`.
3. **CCCI Speech Mailbox (`/dev/ccci_aud`, CCIF0 Q0 `ch=4 RX` / `ch=5 TX`):**
   - Verified from `audio.primary.mediatek.so` (`SpeechMessengerNormal` / `SpeechDriverNormal`):
     - Uses `PORT_F_USER_HEADER` preserving `hdr.data[0]`, `hdr.data[1]`, `hdr.reserved`.
     - **Mailbox packet (16 B):** `data[0] = 0xffffffff`, `data[1] = (msg_id << 16) | param16`,
       `channel = 5`, `reserved = param32`.
     - **Payload packet:** `data[0] = 0`, `data[1] = payload_len + 6`,
       `channel = 5`, `reserved = (msg_id << 16) | (payload_len + 6)`, payload starts
       with `[u16 0xa2a2, u16 data_type, u16 payload_len, data...]`.
     - **Key message IDs:**
       - `MSG_A2M_SPH_ON = 0x2f20` (150 B payload, `data_type = 0x0019`, 128 B `mtk_sph_info` -> MD replies `0xaf20`)
       - `MSG_A2M_SPH_OFF = 0x2f21` (16 B mailbox, `param16 = 0`, `param32 = 0` -> MD replies `0xaf21`)
       - `MSG_A2M_SET_SPEECH_MODE = 0x2f2b` (150 B payload, `data_type = 0x0019`, 128 B `mtk_sph_info`)
       - `MSG_A2M_MD_ALIVE_ACK = 0x2fa0` (16 B mailbox reply to `0xafa0`)
       - `MSG_M2A_MD_ALIVE = 0xaf70` (boot-time mailbox hello from MD)
       - `MSG_M2A_EM_DATA_REQUEST / SPEECH_VER = 0xafa0` (boot-time payload from MD)

### Wave 2 Userspace Speech Bridge (`ccci_audd`)
- Implemented in [`tools/ccci_audd/ccci_audd.c`](../tools/ccci_audd/ccci_audd.c) and packaged in
  `advan-x1-support` as `ccci-audd.service`.
- Monitors ModemManager `org.freedesktop.ModemManager1.Modem.Voice` / `.Call` state transitions
  over system D-Bus; when a call enters `DIALING` / `RINGING_OUT` / `ACTIVE` / `HELD`:
  1. Switches ALSA UCM2 (`mt6789-mt6366`) to `Voice Call` (`Earpiece` / `Speaker` / `Headphones` + `Mic`,
     keeping `Lineout Volume <= 8`).
  2. Opens and starts `Hostless_Speech` (`/dev/snd/pcmC0D5p` + `/dev/snd/pcmC0D5c` at 32 kHz using
     `sw.stop_threshold = ULONG_MAX` so `SNDRV_PCM_IOCTL_START` activates the PCM2<->ADDA crossbar
     without any DMA transfer).
  3. Sends `MSG_A2M_SPH_ON` (`0x2f20`, 150 B payload) over `/dev/ccci_aud` and verifies the
     `MSG_M2A_SPH_ON_ACK` (`0xaf20`) reply from MD1.
- On call termination, sends `MSG_A2M_SPH_OFF` (`0x2f21`), closes `Hostless_Speech`, and restores
  UCM2 `HiFi`.

---

## Stage 3: SMS (3GPP PDU Mode)

### Stock Behavior (`libmtk-ril.so` + `radio.log`)
- Initialization:
  - `AT+CMGF=0` (PDU mode)
  - `AT+CSMS=1` (Phase 2+, expects `+CSMS: 1, 1, 1`)
  - `AT+CNMI=1,2,2,1,1` (direct URC delivery via `+CMT:`)
  - `AT+CPMS="SM","SM","SM"` (SIM storage; resolves ModemManager's
    `cannot set default storage` warning on MOLY)
- Send / ACK: `AT+CMGS=<len>` + PDU, ACK received SMS via `AT+CNMA=1,2,"0000"`.
- Wired into `mm-plugin-mtk-ccci.c`:
  - `power_up_radio_ready()` uses a non-aborting `optional_at_continue()` response
    processor so every CS voice and SMS AT command executes cleanly.
  - Implements `MMIfaceModemMessaging::load_supported_storages` advertising
    `MM_SMS_STORAGE_SM` (`mem1`/`mem2`/`mem3`) so ModemManager always configures
    SIM storage cleanly even when `+CPMS=?` is unavailable prior to `+CFUN=1`.

---

## Stage 4: VoLTE / IMS (Architecture Spec)

### Stock Android Architecture (Verified on Advan X1)
Stock MediaTek Gen95 VoLTE does **not** terminate SIP inside the modem; instead,
four proprietary vendor userspace daemons run on AP Linux and bridge SIP/RTP/IPsec
to MD1 over dedicated CCCI channels:
- `volte_imsm_93` (`/dev/ccci_imsm`, ch `168/169`) — IMS media / RTP / speech bridge
- `volte_imcb` (`/dev/ccci_imsa`, ch `56/57`; `/dev/ccci_imsdc`, ch `58/59`) —
  IMS control bridge translating MD1 IMS indications to `volte_stack` / `volte_ua`
- `volte_stack` & `volte_ua` — SIP/IMS user agent, AKA auth, IPsec SA setup
- AT commands (`libmtk-ril.so` `RmcImsControlRequestHandler`):
  - `AT+EIMSPDN="onoff",1` -> `AT+EAPNACT=1,"ims","ims",0` (separate IMS PDN on
    a second DPMAIF `net_if` / `wwan1`)
  - `AT+EIMSCFG=1,1,0,0,1,1` (enable VoLTE/ViLTE/WFC/SMSoIP)
  - `AT+CIREG=2` (IMS registration URC)

### Mainline Status & Path Forward
- **Wave 1 (Done):** Kernel `mtk_ccci_core.c` cleanly handles `CCCI_CH_SYSTEM_RX`
  (`ch=2`) and drains `CCCI_CH_IMSA_DL` (`57`), `CCCI_CH_IMSDC_DL` (`59`),
  `CCCI_CH_IMSM_RX` (`169`), and `CCCI_CH_WOA_RX` (`171`) with zero unhandled
  channel warnings.
- **Stage 4 (Future):** Requires either an open-source IMS/SIP stack (e.g.
  `imsd` / PJSIP + XFRM IPsec + SIM ISIM AKA over `AT+CSIM`) talking to a second
  DPMAIF WWAN interface (`wwan1`, `ims` APN) plus `/dev/ccci_imsa` & `/dev/ccci_imsm`
  chardevs, once Stages 1-3 are verified rock-solid on hardware.
