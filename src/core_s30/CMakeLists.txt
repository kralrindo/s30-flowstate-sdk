cmake_minimum_required( VERSION 3.16 )

# S30 client product: client_s30.dll, loaded by loader.dll when the host is a
# known S30 image. It starts small -- log, loopback-only network guard, offline
# platform watchdog -- and takes over SDK subsystems one at a time as each is
# verified against the S30 image. It does not link the S21 *_cl libraries: their
# dual-ABI layouts (ConVar, sqvm, ...) are S21's until proven otherwise.
#
# REUSE_PCH is empty (third arg): no stdafx/shared_pch, like the loader.
add_module( "shared_lib" "client_s30" "" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Core"
    "crash_s30.cpp"
    "crash_s30.h"
    "dllmain_client_s30.cpp"
    "s30_image.h"
    "s30_log.cpp"
    "s30_log.h"
    "s30_pattern.h"
)

add_sources( SOURCE_GROUP "Platform"
    "net_guard_s30.cpp"
    "net_guard_s30.h"
    "offline_s30.cpp"
    "offline_s30.h"
    "origin_offline_s30.cpp"
    "origin_offline_s30.h"
)

target_link_libraries( ${PROJECT_NAME} PRIVATE
    "ws2_32.lib"
    "shlwapi.lib"
    "libdetours"
)

end_sources()

r5sdk_attach_pe_identity( ${PROJECT_NAME} )
