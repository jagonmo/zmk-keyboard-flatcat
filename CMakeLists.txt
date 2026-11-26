# Only the BLE to USB bridge has sources; the rest of this repo is shields and configs.
if(CONFIG_ZMK_BT_BRIDGE)
  zephyr_library()
  zephyr_library_include_directories(${WEST_TOPDIR}/zmk/app/include)
  zephyr_library_include_directories(bridge)
  zephyr_library_sources(bridge/bt_bridge.c)
endif()
