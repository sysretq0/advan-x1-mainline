# FS1599 speaker hardware protection — Phase A mitigation

**Status:** conservative mitigation applied; full Re25/F0 calibration deferred to Phase B.
**Tier A evidence:** `snd-soc-fs1599.ko`, `fs1599.fsm`, `runtime_merged.dts`, live stock audio sysfs.

## Why the mainline driver cannot do Re25/F0 today

Stock `/work/mainline/ref/advan-x1-oracle/audio_modules/snd-soc-fs1599.ko` exports
(verified with `nm -n`) the measurement/calibration functions:

| Symbol | Offset | Role |
|---|---:|---|
| `fsm_cal_spkr_zmimp` | `0x36ac` | speaker impedance measurement |
| `fsm_set_re25_data` | `0x4050` | per-unit Re25 coefficient load |
| `fsm_check_spkcoef` | `0x13c0` | coefficient validation |
| `fsm_stub_pre_calib` / `fsm_stub_post_calib` | `0x3ba8` / `0x3c44` | calibration hooks |
| `fsm_stub_pre_f0` / `fsm_stub_post_f0` | `0x3ce0` / `0x3d7c` | F0 resonance hooks |
| `fsm_stub_wait_test` / `fsm_stub_get_livedata` | `0x3e18` / `0x3f10` | live measurement |

The measurement data comes from the MediaTek SCP DSP over Audio IPI:
`mtk-sp-spk-amp.ko` imports `audio_send_ipi_buf_to_dsp` /
`audio_recv_ipi_buf_to_dsp` and depends on `snd-soc-fs1599,audio_ipi,
mtk-scp-audio,snd-soc-audiodsp-common`. Stock DTB enables it via
`runtime_merged.dts:6100` `scp_spk_process_enable = <0x00 0x04 0x0f 0x13>`
and `mt6789-mt6366.c` DAI link `"SCP_SPK_Process"`.

`fs1599.fsm` is 920 bytes of closed-source FourSemi register/coefficient
preset data (header `00 01 66 73 ...`, DEVID `0x21a1` at `0x3c`), parsed in
kernel by `fsm_parse_preset()` after `request_firmware()`. It is **not** a
portable measurement algorithm.

Pristine upstream `/work/mainline/linux` has no `mediatek,snd_scp_audio`
driver, no `audio_ipi`, and no SCP speaker task firmware. A real port needs
all of those plus the vendor DSP task binary — Phase B work, not a stable
remediation.

## Physical risk being mitigated

Without per-unit Re25/F0, the driver applies a fixed nominal 8 Ω Class-K
boost profile (`0035`: reg `0x59`=`0x884f`→`0x084f`, `0x5a`=`0x80cd`→`0x80ba`,
`BSTCTRL=0x0010`, `AMPCTRL_ENABLE=0x00c0` at reg `0x11`). A lower-Re unit or
one whose F0 shifted downward can then see excess cone excursion at
resonance and I²R voice-coil heating. Stock prevents this by measuring the
actual transducer and adapting the drive.

## Phase A mitigation (applied)

Reduce MT6366 LINEOUT drive into the FS1599 from 0 dB to −200 mB:

- `tools/ucm2/MediaTek/mt6789-mt6366/HiFi.conf`:
  `cset "name='Lineout Volume' 10"` → `8`.
  Scale from `sound/soc/codecs/mt6358.c`: `SOC_DOUBLE_EXT_TLV("Lineout Volume",
  MT6358_ZCD_CON1, 0, 7, 0x12, ...)` with 100 mB steps — value 10 = 0 dB,
  max 18 = +800 mB, 8 = −200 mB. The speaker path enables `LOL Mux` at
  `HiFi.conf:42` and feeds FS1599, so this caps the analog input to the amp.
- A note in patch `0035` records why no further gain change is made there:
  the driver has no FS1599-side gain control (`NULL, 0` controls at
  `0035:342-343`), and blindly editing its static 8 Ω profile is not
  datasheet-backed.

This costs up to 200 mB of speaker level but bounds voice-coil power for
every unit. It is userspace-only and fully reversible.

## Phase B exit criteria

1. Port `mediatek,snd_scp_audio` binding + driver and `audio_ipi`.
2. Load vendor `scp_rv55.bin` speaker task; bind `"SCP_SPK_Process"`.
3. Wire `fsm_cal_spkr_zmimp` / Re25 / F0 to SCP live data.
4. Re-raise `Lineout Volume` to stock 10 only after measured Re/F0 protection
   is proven active (sysfs telemetry + thermal/voice-coil check).
