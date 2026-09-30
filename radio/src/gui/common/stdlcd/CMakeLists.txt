# Render framework kept in all builds (needed for the minimal home view + safety popups + calibration).
set(GUI_SRC
  ${GUI_SRC}
  ../common/stdlcd/menus_common.cpp
  ../common/stdlcd/draw_functions.cpp
  ../common/stdlcd/fonts.cpp
  ../common/stdlcd/utf8.cpp
  ../common/stdlcd/popups_common.cpp
  ../common/stdlcd/calibration.cpp
  ../common/stdlcd/splash.cpp
  ../common/stdlcd/widgets_common.cpp
  ../common/stdlcd/lcd_common.cpp
  )

# model_inputs / model_mixes are kept even in UI_COMPANION: besides the editor UI they define the
# model-data helpers the companion's list-ops reuse (getExposCount/insertExpo/deleteExpo/getMixCount/
# insertMix/deleteMix) plus the shared copy-state (s_currCh, …). Extracting those helpers into a core
# model file is the clean follow-up that would let these drop too.
set(GUI_SRC
  ${GUI_SRC}
  ../common/stdlcd/model_inputs.cpp
  ../common/stdlcd/model_mixes.cpp
  )

# Native config-editor menus + views — replaced by the UltraEdge phone UI in UI_COMPANION.
if(NOT UI_COMPANION)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/menu_model.cpp
    ../common/stdlcd/menu_radio.cpp
    ../common/stdlcd/view_text.cpp
    ../common/stdlcd/model_notes.cpp
    ../common/stdlcd/model_curves.cpp
    ../common/stdlcd/radio_hardware.cpp
    ../common/stdlcd/radio_version.cpp
    ../common/stdlcd/radio_trainer.cpp
    ../common/stdlcd/view_telemetry.cpp
    ../common/stdlcd/radio_sdmanager.cpp
    model_curve_edit.cpp
    )
endif()

if((PXX2 OR LUA OR MULTIMODULE) AND NOT UI_COMPANION)
  set(GUI_SRC ${GUI_SRC} ../common/stdlcd/radio_tools.cpp)
endif()

if(HELI AND NOT UI_COMPANION)
  set(GUI_SRC ${GUI_SRC} ../common/stdlcd/model_heli.cpp)
endif()

if(PXX2 AND NOT UI_COMPANION)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/radio_power_meter.cpp
    )
endif()

if(GHOST AND NOT UI_COMPANION)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/radio_ghost_menu.cpp
    )
endif()

if((PXX2 OR MULTIMODULE) AND NOT UI_COMPANION)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/radio_spectrum_analyser.cpp
    )
endif()

if(PXX2 AND NOT UI_COMPANION)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/model_module_options.cpp
    ../common/stdlcd/model_receiver_options.cpp
    )
endif()

if(USBJ_EX AND NOT UI_COMPANION)
  set(GUI_SRC
    ${GUI_SRC}
    ../common/stdlcd/model_usbjoystick.cpp
    )
endif()

