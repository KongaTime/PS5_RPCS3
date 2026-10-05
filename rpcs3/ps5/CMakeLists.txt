# PS5: RPCS3's frontend on the console (ps5_frontend.h), linked by the title
add_library(rpcs3_ps5 STATIC
    ps5_frontend.cpp
    ps5_gs_frame.cpp
)
target_link_libraries(rpcs3_ps5 PUBLIC rpcs3_emu)
target_compile_options(rpcs3_ps5 PRIVATE "SHELL:-include ${PS5_VOLK_DIR}/volk.h")
