# SPDX-License-Identifier: Apache-2.0
#
# reSpeaker Clip sysbuild module glue.
#
# Applications targeting clip/nrf5340/cpuapp need no per-app sysbuild/ folder:
# the board's Kconfig.sysbuild supplies every SB_CONFIG_* default, and this
# glue points the mcuboot + ipc_radio images at the board's shared sysbuild
# config. A sample is therefore just CMakeLists.txt + prj.conf + src/ — and it
# still boots under the custom, signed MCUboot (OLED, USB CDC, SD erase, PMIC).
#
# An app may still override any of these by providing its own
# <app>/sysbuild/<image>.{conf,overlay} (checked first); the board files are
# only the fallback.
#
# sysbuild_add_subdirectory(images) adds the mcuboot/ipc_radio images (and runs
# their <app>/sysbuild/ discovery) before including this glue, but only
# configures them afterwards — so setting these cache vars here takes effect.

if(NOT BOARD MATCHES "^clip")
  return()
endif()

if(SB_CONFIG_CLIP_WIFI_PATCH_FACTORY_ONLY AND
   (SB_CONFIG_DFU_ZIP_WIFI_FW_PATCH OR SB_CONFIG_DFU_MULTI_IMAGE_PACKAGE_WIFI_FW_PATCH))
  message(FATAL_ERROR
    "Factory-only WiFi patch storage has no MCUboot patch slots. "
    "Disable patch DFU packaging or implement a complete patch-update layout.")
endif()

set(_clip_sysbuild_dir "${SYSBUILD_CURRENT_MODULE_DIR}/boards/seeed/clip/sysbuild")

# Custom MCUboot config (OLED, USB CDC serial recovery, custom mcumgr commands,
# PMIC, SD erase). Used unless the app provides its own.
if(NOT EXISTS ${APP_DIR}/sysbuild/mcuboot.conf)
  set(mcuboot_EXTRA_CONF_FILE  "${_clip_sysbuild_dir}/mcuboot.conf"     CACHE INTERNAL "Clip board mcuboot Kconfig fragment")
endif()
if(NOT EXISTS ${APP_DIR}/sysbuild/mcuboot.overlay)
  set(mcuboot_DTC_OVERLAY_FILE "${_clip_sysbuild_dir}/mcuboot.overlay"  CACHE INTERNAL "Clip board mcuboot DTS overlay")
endif()

# Network-core IPC BLE controller config. Used unless the app overrides it.
if(NOT EXISTS ${APP_DIR}/sysbuild/ipc_radio/prj.conf)
  set(ipc_radio_EXTRA_CONF_FILE "${_clip_sysbuild_dir}/ipc_radio/prj.conf" CACHE INTERNAL "Clip board ipc_radio Kconfig fragment")
endif()
