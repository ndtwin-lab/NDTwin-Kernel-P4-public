# src/ndt_core/http/CMakeLists.txt

# 1. define a static lib for all http‑session code
add_library(NdtCore_HttpLib
  HttpSession.cpp
)

# 2. make sure the compiler can find our public headers
target_include_directories(NdtCore_HttpLib
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# 3. link in any dependencies needed by HttpSession.cpp
#    Beast + ASIO are header‑only, but you may need boost_system etc.
#    We also depend on our other internal libs:
target_link_libraries(NdtCore_HttpLib
  PUBLIC
    UtilsLib
    EventSystemLib
    NdtCore_CollectionLib
    NdtCore_EventHandlingLib
    # (and any other NdtCore_* libs you call in your handlers)
)