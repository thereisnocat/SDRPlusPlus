cmake_minimum_required(VERSION 3.13)
project(rsr200_source)

# transport_usb.cpp compiles on Windows, Linux and macOS -- Windows against FTDI's WinUSB
# D3XX SDK (FTD3XX.h), the other two against FTDI's POSIX D3XX SDK (ftd3xx.h), which is a
# different library with a different async-I/O call for the same job (see transport_usb.cpp).
# The LAN transport (RSR200_PLAN.md phase 2) is header-only (src/rsr200_lan_transport.h,
# raw BSD/Winsock sockets, no vendor SDK) so it needs no separate source file here.
set(SRC "src/main.cpp" "src/transport_usb.cpp")

include(${SDRPP_MODULE_CMAKE})

if (MSVC)
    # FTD3XX SDK: header/import lib/runtime dll installed from FTDI's Winusb_D3XX_Release
    # package to C:/Program Files/FTD3XX, matching the RtAudio/PothosSDR install pattern
    # already used by audio_sink and the other vendor-SDK modules in this tree.
    target_include_directories(rsr200_source PRIVATE "C:/Program Files/FTD3XX/include")
    target_link_directories(rsr200_source PRIVATE "C:/Program Files/FTD3XX/lib")
    target_link_libraries(rsr200_source PRIVATE FTD3XXWU)
else ()
    # FTDI ships the Linux and macOS D3XX SDK as a bare tarball, not a package -- no
    # pkg-config .pc file, no Homebrew formula. Its own install instructions (both
    # platforms, identically) put ftd3xx.h/Types.h under /usr/local/include and
    # libftd3xx.{so,dylib} under /usr/local/lib; that's the only convention there is to
    # follow, matching how the Mac side of this project already had it installed for
    # phase 0 (see ENGINEERING_NOTES.md's note on libftd3xx.dylib's install name).
    target_include_directories(rsr200_source PRIVATE "/usr/local/include")
    target_link_directories(rsr200_source PRIVATE "/usr/local/lib")
    target_link_libraries(rsr200_source PRIVATE ftd3xx)
endif ()
