cmake_minimum_required(VERSION 3.13)
project(rsr200_source)

# transport_usb.cpp is Windows-only (it's built directly on the D3XX WinUSB API), and no
# other transport exists yet -- transport_lan.cpp is RSR200_PLAN.md phase 2, not started.
# So for now the whole module is Windows-only; see the option's guard in the top-level
# CMakeLists.txt. main.cpp only ever constructs rsr200::UsbTransport, so nothing here needs
# an #ifdef for platform: the module simply isn't offered to build elsewhere yet.
set(SRC "src/main.cpp" "src/transport_usb.cpp")

include(${SDRPP_MODULE_CMAKE})

if (MSVC)
    # FTD3XX SDK: header/import lib/runtime dll installed from FTDI's Winusb_D3XX_Release
    # package to C:/Program Files/FTD3XX, matching the RtAudio/PothosSDR install pattern
    # already used by audio_sink and the other vendor-SDK modules in this tree.
    target_include_directories(rsr200_source PRIVATE "C:/Program Files/FTD3XX/include")
    target_link_directories(rsr200_source PRIVATE "C:/Program Files/FTD3XX/lib")
    target_link_libraries(rsr200_source PRIVATE FTD3XXWU)
endif ()
