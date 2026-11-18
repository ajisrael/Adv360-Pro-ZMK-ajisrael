if(CONFIG_ZMK_HOST_SELECT)
  # Part of the ZMK app target, so ZMK's own headers are on the include path.
  target_sources(app PRIVATE src/host_select.c)
endif()
