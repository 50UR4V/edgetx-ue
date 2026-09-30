set(GUI_SRC
  ${GUI_SRC}
  lcd.cpp
  splash.cpp
  popups.cpp
  widgets.cpp
  startup_shutdown.cpp
  model_select.cpp
  radio_calibration.cpp
  view_main.cpp
)

# UltraEdge companion UI mode: the native config-editor menus are replaced by the phone's describe
# pages, so drop them. Keep model_select + calibration + view_main (basics / safety / first-boot).
if(NOT UI_COMPANION)
  set(GUI_SRC ${GUI_SRC}
    model_setup.cpp
    model_failsafe.cpp
    model_logical_switches.cpp
    model_special_functions.cpp
    model_outputs.cpp
    model_telemetry.cpp
    model_telemetry_sensor.cpp
    radio_setup.cpp
    view_statistics.cpp
  )
endif()

if(FLIGHT_MODES AND NOT UI_COMPANION)
  set(GUI_SRC ${GUI_SRC}
    model_flightmodes.cpp
  )
endif()

if(GVARS AND NOT UI_COMPANION)
  set(GUI_SRC ${GUI_SRC}
    model_gvars.cpp
  )
endif()

if(LUA_MIXER AND NOT UI_COMPANION)
  set(GUI_SRC ${GUI_SRC}
    model_custom_scripts.cpp
  )
endif()

if(UI_COMPANION)
  set(GUI_SRC ${GUI_SRC}
    ui_companion_stubs.cpp
  )
endif()

include(gui/common/stdlcd/CMakeLists.txt)

set(BOOTLOADER_SRC ${BOOTLOADER_SRC}
  ${RADIO_SRC_DIR}/gui/128x64/lcd.cpp
  ${RADIO_SRC_DIR}/gui/common/stdlcd/lcd_common.cpp
  ${RADIO_SRC_DIR}/gui/common/stdlcd/fonts.cpp
  ${RADIO_SRC_DIR}/gui/common/stdlcd/utf8.cpp
)
