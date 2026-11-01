set(GUI_SRC
  ${GUI_SRC}
  ../common/stdlcd/menu_model.cpp
  ../common/stdlcd/menu_radio.cpp
  ../common/stdlcd/menus_common.cpp
  ../common/stdlcd/draw_functions.cpp
  ../common/stdlcd/fonts.cpp
  ../common/stdlcd/utf8.cpp
  ../common/stdlcd/popups_common.cpp
  ../common/stdlcd/model_inputs.cpp
  ../common/stdlcd/model_mixes.cpp
  ../common/stdlcd/view_text.cpp
  ../common/stdlcd/model_notes.cpp
  ../common/stdlcd/model_curves.cpp
  ../common/stdlcd/radio_hardware.cpp
  ../common/stdlcd/radio_version.cpp
  ../common/stdlcd/radio_trainer.cpp
  ../common/stdlcd/view_telemetry.cpp
  ../common/stdlcd/calibration.cpp
  ../common/stdlcd/splash.cpp
  ../common/stdlcd/radio_sdmanager.cpp
  ../common/stdlcd/widgets_common.cpp
  ../common/stdlcd/lcd_common.cpp
  model_curve_edit.cpp
  )

if(PXX2 OR LUA OR MULTIMODULE)
  set(GUI_SRC ${GUI_SRC} ../common/stdlcd/radio_tools.cpp)
endif()

if(HELI)
  set(GUI_SRC ${GUI_SRC} ../common/stdlcd/model_heli.cpp)
endif()

if(PXX2)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/radio_power_meter.cpp
    )
endif()

if(GHOST)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/radio_ghost_menu.cpp
    )
endif()

if(PXX2 OR MULTIMODULE)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/radio_spectrum_analyser.cpp
    )
endif()

if(PXX2)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/model_module_options.cpp
    ../common/stdlcd/model_receiver_options.cpp
    )
endif()

if(USBJ_EX)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/model_usbjoystick.cpp
    )
endif()

# UltraEdge companion UI mode: drop the native config menus/views + calibration screen (see gui/128x64/CMakeLists).
# model_inputs/model_mixes stay: they also hold the model-data helpers the companion's list ops reuse.
# Same named groups as gui/128x64/CMakeLists.txt (ADR-0017).
set(UE_TRIM_STD_MODEL_MENUS ../common/stdlcd/menu_model.cpp ../common/stdlcd/model_notes.cpp ../common/stdlcd/model_curves.cpp
    model_curve_edit.cpp ../common/stdlcd/model_heli.cpp ../common/stdlcd/model_module_options.cpp
    ../common/stdlcd/model_receiver_options.cpp ../common/stdlcd/model_usbjoystick.cpp)
set(UE_TRIM_STD_RADIO_MENUS ../common/stdlcd/menu_radio.cpp ../common/stdlcd/radio_hardware.cpp ../common/stdlcd/radio_version.cpp
    ../common/stdlcd/radio_trainer.cpp ../common/stdlcd/radio_sdmanager.cpp ../common/stdlcd/radio_tools.cpp
    ../common/stdlcd/radio_power_meter.cpp ../common/stdlcd/radio_ghost_menu.cpp ../common/stdlcd/radio_spectrum_analyser.cpp)
set(UE_TRIM_STD_CALIBRATION ../common/stdlcd/calibration.cpp)
set(UE_TRIM_STD_VIEWS ../common/stdlcd/view_text.cpp ../common/stdlcd/view_telemetry.cpp)
if(UI_COMPANION)
  list(REMOVE_ITEM GUI_SRC ${UE_TRIM_STD_MODEL_MENUS} ${UE_TRIM_STD_RADIO_MENUS} ${UE_TRIM_STD_CALIBRATION} ${UE_TRIM_STD_VIEWS})
endif()
