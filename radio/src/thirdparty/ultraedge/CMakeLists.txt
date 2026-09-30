# UltraEdge companion protocol — firmware-embedded build.
# Compiled into the firmware only when USB_COMPANION is defined. Embedded-safe:
# no dynamic allocation, no exceptions, no RTTI (inherits firmware flags).
add_library(ultraedge_protocol STATIC protocol.cpp)
target_include_directories(ultraedge_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
