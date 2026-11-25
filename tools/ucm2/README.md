# ALSA UCM2 Profiles for Advan X1 (MT6789 + MT6366 + FourSemi FS1599)

These UCM2 profiles wire PipeWire / PulseAudio / ALSA directly to the
`mt6789-mt6366` ASoC sound card on mainline Linux:

- **Speaker:** `DL1` (`hw:CardId,0`) → `ADDA` → MT6366 `LOL` (`LINEOUT L`) → FourSemi FS1599 (`i2c6@0x34`).
- **Headphones:** `DL1` (`hw:CardId,0`) → `ADDA` → MT6366 `HPL`/`HPR`, auto-switched via `Headset Jack` (`mt6358-accdet`).
- **Earpiece:** `DL1` (`hw:CardId,0`) → `ADDA` → MT6366 `RCV` (`Receiver`).
- **Internal Mic:** `AIN0` → MT6366 `PGA`/`ADC` → `ADDA_UL` → `UL1` (`hw:CardId,3`).
- **Headset Mic:** `AIN1` → MT6366 `PGA`/`ADC` → `ADDA_UL` → `UL1` (`hw:CardId,3`), auto-switched via `Headset Jack`.

## Install

```bash
install -d /usr/share/alsa/ucm2/MediaTek/mt6789-mt6366 \
           /usr/share/alsa/ucm2/conf.d/mt6789-mt6366
install -m 644 MediaTek/mt6789-mt6366/mt6789-mt6366.conf \
               MediaTek/mt6789-mt6366/HiFi.conf \
               /usr/share/alsa/ucm2/MediaTek/mt6789-mt6366/
ln -sf ../../MediaTek/mt6789-mt6366/mt6789-mt6366.conf \
       /usr/share/alsa/ucm2/conf.d/mt6789-mt6366/mt6789-mt6366.conf
```
