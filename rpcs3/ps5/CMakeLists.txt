# PS5: RPCS3's frontend on the console (ps5_frontend.h), linked by the title.
# With it, what the desktop builds into its Qt side and the emulator needs
# without Qt: the pad thread and its tables, the PS Move configuration and
# tracker (without OpenCV), and the version strings.
add_library(rpcs3_ps5 STATIC
    ps5_frontend.cpp
    ps5_gs_frame.cpp
    ps5_pad_handler.cpp
    ../Input/pad_thread.cpp
    ../Input/product_info.cpp
    ../Input/ps_move_config.cpp
    ../Input/ps_move_tracker.cpp
    ../rpcs3_version.cpp
)
target_link_libraries(rpcs3_ps5 PUBLIC rpcs3_emu 3rdparty::fusion) # Fusion: the pad thread's motion filter
target_compile_options(rpcs3_ps5 PRIVATE "SHELL:-include ${PS5_VOLK_DIR}/volk.h")
