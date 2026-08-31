# src/ndt_core/event_handling/CMakeLists.txt

add_library(NdtCore_EventHandlingLib STATIC
    ControllerAndOtherEventHandler.cpp
)

# Tell this library where to find <libssh/libssh.h> so it can COMPILE.
# The PUBLIC keyword is important as it propagates this setting.
target_include_directories(NdtCore_EventHandlingLib PUBLIC
    ${LIBSSH_INCLUDE_DIRS}
)