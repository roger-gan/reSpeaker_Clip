# Experimental nRF7002 patch storage on external SPI flash

This branch tests the NCS external-flash STORE loader on Clip. It is not an
ordinary device upgrade: the new layout shrinks LittleFS and requires a
separately provisioned Wi-Fi patch. No device has been flashed or tested yet.

## Build results (2026-09-20)

Base: `d508d21e0c5c872bff3886cb27e7807826ceb90b`, NCS 3.3.0,
Zephyr SDK 0.17.0, Production snippet. The complete sysbuild succeeded.

| Application resource | Baseline | Experiment |
|---|---:|---:|
| Flash used | 930,708 B | 844,044 B |
| Flash remaining | 2,668 B | 89,332 B |
| RAM statically reserved | 430,930 B | 430,914 B |

Net internal Flash savings: **86,664 B (84.63 KiB)**. Remaining application
Flash: **87.24 KiB**. Static RAM changes by only -16 B; temporary loader heap
usage is not included in that delta. The configured system heap remains 80 KiB.

Read-only checks passed for final configuration, unchanged application/OTA
region sizes, external partition bounds and alignment, absence of embedded
`fw_patch`, and inclusion of the SDK loader object. LTO inlines loader functions,
so checks use the map and configuration as well as ELF symbols.

The generated patch HEX matches the raw SDK binary byte-for-byte (87,046 B),
SHA-256 `6353a2d07319337a5aaa75af03f2303756496eda0e2bea53ef47bc2148f64e7f`.
Its generated address starts at **0x107e0000**, not the SPI device offset
0x7e0000. This artifact does not prove a flash tool can program Clip's SPI3 NOR.
The DFU ZIP contains only `clip.signed.bin`, `ipc_radio.bin` and `manifest.json`;
it does not contain the separately provisioned patch.

Reproduce build checks in the NCS Python environment:

```sh
python /home/ubuntu/firmware/reSpeaker_Clip/scripts/verify_external_wifi_patch.py \
  /home/ubuntu/firmware/ncs/build-clip-prod \
  /home/ubuntu/firmware/ncs/build-clip-ext-patch \
  /home/ubuntu/firmware/ncs/nrfxlib/nrf_wifi/bin/ncs/default/nrf70.bin
```

The verifier also requires `intelhex`, available in the NCS environment.

## Configuration

- `applications/clip/sysbuild.conf` selects
  `SB_CONFIG_WIFI_PATCHES_EXT_FLASH_STORE=y`.
- NCS otherwise assumes external patches have a third MCUboot update slot.
  The board-local `CLIP_WIFI_PATCH_FACTORY_ONLY` experiment overrides that
  default and excludes the patch from the DFU ZIP, preserving the existing
  two-image bootloader. Patch DFU must not be enabled in this configuration.
- The application uses 2048-byte patch reads and retains SDK SHA-256 integrity
  checking. The SDK reads from the flash area into a temporary heap buffer and
  downloads each chunk to nRF7002; it does not retain the entire patch in the
  application core's RAM.
- The board's Partition Manager YAML and DTS reserve `0x7e0000..0x800000` for
  `nrf70_wifi_fw` (128 KiB). LittleFS remains at `0x130000`, but its size changes
  from `0x6d0000` to `0x6b0000`. Both existing OTA slots retain their addresses
  and sizes. Internal partitions are not intentionally changed.
- Clip's storage flash uses SPI3, while nRF7002 uses QSPI. STORE is selected;
  this experiment does not enable external-flash XIP.

## Build on the existing Ubuntu server

```sh
source /home/ubuntu/firmware/.venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=/home/ubuntu/firmware/zephyr-sdk-0.17.0
export ZEPHYR_EXTRA_MODULES=/home/ubuntu/firmware/reSpeaker_Clip
cd /home/ubuntu/firmware/ncs
west build --sysbuild -b clip/nrf5340/cpuapp \
  /home/ubuntu/firmware/reSpeaker_Clip/applications/clip \
  -d build-clip-ext-patch -S production -- \
  -DSNIPPET_ROOT=/home/ubuntu/firmware/reSpeaker_Clip/applications/clip
```

Keep `build-clip-prod` as the unmodified comparison artifact. Record the source
base commit and the experimental diff alongside the new ELF, map, final
configuration, partitions and build log. Compare actual linked Flash/RAM usage,
not ELF file size or just the removed patch symbol.

## Provisioning and upgrade boundary

Before any hardware trial:

1. Back up existing device configuration. Shrinking a mounted LittleFS without
   migration/recreation can expose its existing data to patch writes; changing
   the partition definition alone does not resize its on-disk filesystem.
2. Check the generated partition table and the selected SDK patch mode. Retain
   the matching raw SDK `nrf70.bin`, with its original header and integrity hash.
3. Implement or validate a writer that accesses Clip's actual SPI NOR driver
   and writes the raw patch to the dedicated partition, then reads it back and
   verifies it. A normal application-only USB update does not provision it.
4. Do not assume Nordic's generated external HEX is directly flashable on Clip.
   Its logical address and the flash tool's access to ordinary SPI3 NOR must be
   checked separately; QSPI address mapping does not establish that access.
5. Only after filesystem migration and patch provisioning, test cold boot,
   repeated Wi-Fi start/stop and file transfer. Check behavior for absent or
   corrupt patch data, including successful local recording when Wi-Fi fails.

This branch does not add patch DFU slots, increase MCUboot's image count, change
bootloader keys, or implement automatic filesystem migration. A product update
needs explicit patch/application compatibility and interrupted-update recovery.
Rollback to a prior firmware also needs to account for the changed filesystem
geometry; do not assume reflashing the old application restores that geometry.

## Validation limits

Build verification can establish configuration propagation, partition bounds,
artifact consistency, symbol removal and linked resource savings. Actual SPI
reads, provisioning, Wi-Fi startup, runtime heap peaks and power consumption
require a device. No firmware should be described as hardware-validated based
only on these build checks.
