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
    "concommand_s30.cpp"
    "concommand_s30.h"
    "console_s30.cpp"
    "console_s30.h"
    "crash_s30.cpp"
    "crash_s30.h"
    "dllmain_client_s30.cpp"
    "engine_log_s30.cpp"
    "engine_log_s30.h"
    "script_print_s30.cpp"
    "script_print_s30.h"
    "s30_image.h"
    "s30_log.cpp"
    "s30_log.h"
    "s30_pattern.h"
    "sdk_commands_s30.cpp"
    "sdk_commands_s30.h"
)

add_sources( SOURCE_GROUP "Overlay"
    "overlay_panels_s30.cpp"
    "overlay_s30.cpp"
    "overlay_s30.h"
)

add_sources( SOURCE_GROUP "Net"
    "bridge_datablock_s30.cpp"
    "bridge_datablock_s30.h"
    "bridge_handshake_s30.cpp"
    "bridge_handshake_s30.h"
    "bridge_netchan_s30.cpp"
    "bridge_netchan_s30.h"
    "bridge_persistence_s30.cpp"
    "bridge_persistence_s30.h"
    "bridge_usercmd_s30.cpp"
    "bridge_usercmd_s30.h"
    "bridge_scriptremote_s30.cpp"
    "bridge_scriptremote_s30.h"
    "bridge_usermessage_s30.cpp"
    "bridge_usermessage_s30.h"
    "bridge_c2s_s30.cpp"
    "bridge_c2s_s30.h"
    "bridge_activity_s30.cpp"
    "bridge_activity_s30.h"
    "bridge_cmdseed_s30.cpp"
    "bridge_cmdseed_s30.h"
    "bridge_hud_s30.cpp"
    "bridge_hud_s30.h"
    "activity_s3_to_s30_table.h"
    "bridge_signon_s30.cpp"
    "bridge_signon_s30.h"
    "net_diag_s30.cpp"
    "net_diag_s30.h"
)

add_sources( SOURCE_GROUP "Platform"
    "net_guard_s30.cpp"
    "net_guard_s30.h"
    "identity_s30.cpp"
    "identity_s30.h"
    "offline_s30.cpp"
    "offline_s30.h"
    "origin_offline_s30.cpp"
    "origin_offline_s30.h"
)

target_link_libraries( ${PROJECT_NAME} PRIVATE
    "ws2_32.lib"
    "shlwapi.lib"
    "d3d12.lib"
    "dxgi.lib"
    "d3dcompiler.lib"
    "libdetours"
    "libimgui"
    "liblz4"
)

end_sources()

# ImGui backends include "imgui.h" by bare name.
target_include_directories( ${PROJECT_NAME} PRIVATE "${THIRDPARTY_SOURCE_DIR}/imgui/" )

r5sdk_attach_pe_identity( ${PROJECT_NAME} )
