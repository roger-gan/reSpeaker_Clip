#!/usr/bin/env python3
"""Read-only build checks; requires pyelftools and PyYAML from the NCS venv."""

import argparse
import hashlib
import json
from pathlib import Path
from zipfile import ZipFile

import yaml
from elftools.elf.elffile import ELFFile
from intelhex import IntelHex


def config(path):
    return dict(
        line.split("=", 1)
        for line in path.read_text().splitlines()
        if line.startswith(("CONFIG_", "SB_CONFIG_")) and "=" in line
    )


def measure(build):
    parts = yaml.safe_load((build / "partitions.yml").read_text())
    app = parts["app"]
    ram = parts["sram_primary"]
    with (build / "clip/zephyr/zephyr.elf").open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s for s in elf.get_section_by_name(".symtab").iter_symbols()}
        ends = [
            s["p_paddr"] + s["p_filesz"]
            for s in elf.iter_segments()
            if s["p_type"] == "PT_LOAD" and s["p_filesz"]
            and app["address"] <= s["p_paddr"] < app["address"] + app["size"]
        ]
        flash_used = max(ends) - app["address"]
        ram_used = symbols["_end"]["st_value"] - ram["address"]
        result = {
            "flash_used": flash_used,
            "flash_capacity": app["size"],
            "flash_remaining": app["size"] - flash_used,
            "ram_static_used": ram_used,
            "ram_capacity": ram["size"],
            "ram_tail_before_libc_heap": ram["size"] - ram_used,
            "embedded_fw_patch_bytes": symbols["fw_patch"]["st_size"] if "fw_patch" in symbols else 0,
            # LTO can inline the loader functions, so symbol-name absence is
            # not evidence that the external loader was omitted.
            "external_loader_object_in_map": "ext_fw_load.c.obj" in (
                build / "clip/zephyr/zephyr.map"
            ).read_text(),
        }
    return parts, result


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("experiment", type=Path)
    parser.add_argument("patch_binary", type=Path)
    args = parser.parse_args()
    old_parts, old = measure(args.baseline)
    parts, new = measure(args.experiment)
    app_config = config(args.experiment / "clip/zephyr/.config")
    sys_config = config(args.experiment / "zephyr/.config")
    boot_config = config(args.experiment / "mcuboot/zephyr/.config")
    for key, value in {
        "CONFIG_NRF_WIFI_PATCHES_EXT_FLASH_STORE": "y",
        "CONFIG_NRF_WIFI_FW_FLASH_CHUNK_SIZE": "2048",
        "CONFIG_NRF_WIFI_FW_PATCH_INTEGRITY_CHECK": "y",
    }.items():
        require(app_config.get(key) == value, f"{key} != {value}")
    require(app_config.get("CONFIG_NRF_WIFI_PATCHES_EXT_FLASH_XIP") != "y", "XIP selected")
    require(app_config.get("CONFIG_NRF_WIFI_FW_PATCH_DFU") != "y", "patch DFU enabled")
    require(sys_config.get("SB_CONFIG_DFU_ZIP_WIFI_FW_PATCH") != "y", "patch in DFU ZIP")
    require(sys_config.get("SB_CONFIG_MCUBOOT_UPDATEABLE_IMAGES") == "2", "sysbuild image count")
    require(boot_config.get("CONFIG_UPDATEABLE_IMAGE_NUMBER") == "2", "MCUboot image count")
    for name in ("app", "mcuboot", "mcuboot_secondary", "mcuboot_secondary_1"):
        for key in ("address", "size"):
            require(parts[name][key] == old_parts[name][key], f"{name} {key} changed")
    require(parts["nrf70_wifi_fw"]["address"] == 0x7E0000, "patch offset")
    require(parts["nrf70_wifi_fw"]["size"] == 0x20000, "patch size")
    require(parts["lfs_storage"]["address"] == 0x130000, "LittleFS offset")
    require(parts["lfs_storage"]["size"] == 0x6B0000, "LittleFS size")
    external = sorted(
        (p["address"], p["address"] + p["size"], name)
        for name, p in parts.items()
        if p["region"] == "external_flash" and p["size"] and "span" not in p
    )
    previous_end = 0
    for start, end, name in external:
        require(start >= previous_end and end <= 0x800000, f"overlap/bounds: {name}")
        require(start % 0x10000 == 0 and end % 0x10000 == 0, f"alignment: {name}")
        previous_end = end
    require(old["embedded_fw_patch_bytes"] > 0, "baseline patch not found")
    require(new["embedded_fw_patch_bytes"] == 0, "patch still embedded")
    require(new["external_loader_object_in_map"], "external loader object missing")
    require(new["flash_used"] < old["flash_used"], "no Flash savings")
    require(new["flash_remaining"] > 0 and new["ram_tail_before_libc_heap"] > 0, "overflow")
    patch = args.patch_binary.read_bytes()
    require(0 < len(patch) <= parts["nrf70_wifi_fw"]["size"], "patch does not fit")
    patch_hex = IntelHex(str(args.experiment / "nrf70.hex"))
    require(len(patch_hex.addresses()) == len(patch), "HEX patch length mismatch")
    require(bytes(patch_hex.tobinarray()) == patch, "HEX differs from raw SDK patch")
    with ZipFile(args.experiment / "dfu_application.zip") as package:
        package_files = package.namelist()
        require(not any("nrf70" in name.lower() for name in package_files), "patch in ZIP")
    print(json.dumps({
        "status": "PASS: build checks only; not hardware validation",
        "baseline": old,
        "experiment": new,
        "flash_saved": old["flash_used"] - new["flash_used"],
        "ram_static_delta": new["ram_static_used"] - old["ram_static_used"],
        "patch_size": len(patch),
        "patch_sha256": hashlib.sha256(patch).hexdigest(),
        "generated_hex_start": hex(patch_hex.minaddr()),
        "dfu_zip_files": package_files,
        "external_partitions": external,
    }, indent=2))


if __name__ == "__main__":
    main()
