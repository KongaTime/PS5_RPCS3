# PS5: RPCS3's frontend on the console (ps5_frontend.h), linked by the title.
# With it, what the desktop builds into its Qt side and the emulator needs
# without Qt: the pad thread and its tables, the PS Move configuration and
# tracker (without OpenCV), and the version strings.
add_library(rpcs3_ps5 STATIC
    ps5_frontend.cpp
    ps5_gs_frame.cpp
    ps5_pad_handler.cpp
    ps5_firmware.cpp
    ps5_terminate.cpp
    ../Input/pad_thread.cpp
    ../Input/product_info.cpp
    ../Input/ps_move_config.cpp
    ../Input/ps_move_tracker.cpp
    ../rpcs3_version.cpp
)
target_link_libraries(rpcs3_ps5 PUBLIC rpcs3_emu 3rdparty::fusion 3rdparty::zlib) # Fusion: the pad thread's motion filter; zlib: the SCE decrypter (firmware)
target_compile_options(rpcs3_ps5 PRIVATE "SHELL:-include ${PS5_VOLK_DIR}/volk.h")
# std::terminate's report rethrows the exception for its message: the one unit
# built with exceptions (ps5_terminate.cpp)
set_source_files_properties(ps5_terminate.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
