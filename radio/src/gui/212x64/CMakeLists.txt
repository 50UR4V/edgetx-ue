set(GUI_SRC
  ${GUI_SRC}
  lcd.cpp
  splash.cpp
  popups.cpp
  widgets.cpp
  startup_shutdown.cpp
  model_select.cpp
  model_setup.cpp
  model_failsafe.cpp
  model_logical_switches.cpp
  model_special_functions.cpp
  model_outputs.cpp
  model_telemetry.cpp
  model_telemetry_sensor.cpp
  radio_setup.cpp
  radio_calibration.cpp
  view_main.cpp
  view_statistics.cpp
  )

if(FLIGHT_MODES)
  set(GUI_SRC
    ${GUI_SRC}
    model_flightmodes.cpp
    )
endif()

if(GVARS)
  set(GUI_SRC ${GUI_SRC}
    model_gvars.cpp
  )
endif()

if(LUA_MIXER)
  set(GUI_SRC
    ${GUI_SRC}
    model_custom_scripts.cpp
    )
endif()

# UltraEdge companion UI mode for the 212x64 GUI (s40, ADR-0041; extends s26 / ADR-0017): same groups as 128x64 —
# the phone does the config, so the radio's config menus go; model select, home view and safety popups stay.
# REMOVE_ITEM keeps the default build's source order (and binary) unchanged. The stubs are shared with 128x64.
set(UE_TRIM_MODEL_MENUS model_setup.cpp model_failsafe.cpp model_logical_switches.cpp model_special_functions.cpp
    model_outputs.cpp model_telemetry.cpp model_telemetry_sensor.cpp model_flightmodes.cpp model_gvars.cpp
    model_custom_scripts.cpp)
set(UE_TRIM_RADIO_MENUS radio_setup.cpp)
set(UE_TRIM_CALIBRATION radio_calibration.cpp)
set(UE_TRIM_VIEWS view_statistics.cpp)
if(UI_COMPANION)
  list(REMOVE_ITEM GUI_SRC ${UE_TRIM_MODEL_MENUS} ${UE_TRIM_RADIO_MENUS} ${UE_TRIM_CALIBRATION} ${UE_TRIM_VIEWS})
  set(GUI_SRC ${GUI_SRC} ../128x64/ui_companion_stubs.cpp)
endif()

include(gui/common/stdlcd/CMakeLists.txt)

set(BOOTLOADER_SRC ${BOOTLOADER_SRC}
  ${RADIO_SRC_DIR}/gui/212x64/lcd.cpp
  ${RADIO_SRC_DIR}/gui/common/stdlcd/lcd_common.cpp
  ${RADIO_SRC_DIR}/gui/common/stdlcd/fonts.cpp
  ${RADIO_SRC_DIR}/gui/common/stdlcd/utf8.cpp
)
