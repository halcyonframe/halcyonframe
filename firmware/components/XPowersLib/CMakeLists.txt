# Vendored verbatim from lewisxhe/XPowersLib (MIT), as shipped in the Waveshare reference firmware.
idf_component_register(
  SRCS "XPowersLibInterface.cpp"
  INCLUDE_DIRS "." "REG"
  REQUIRES driver)
target_compile_definitions(${COMPONENT_LIB} PUBLIC XPOWERS_CHIP_AXP2101 CONFIG_XPOWERS_ESP_IDF_NEW_API)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function)
