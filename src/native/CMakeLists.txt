cmake_minimum_required(VERSION 3.20)
project(CompressorNextNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# C-Laufzeit statisch einbinden: Das Audio-Modul wird vom Windows-Audiodienst geladen und darf
# keine Zusatz-DLLs voraussetzen.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /permissive- /utf-8)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(NOT DEFINED CNX_VERSION)
  set(CNX_VERSION "1.0.0")
endif()
string(REPLACE "." ";" CNX_VERSION_PARTS "${CNX_VERSION}")
list(GET CNX_VERSION_PARTS 0 CNX_VER_MAJOR)
list(GET CNX_VERSION_PARTS 1 CNX_VER_MINOR)
list(GET CNX_VERSION_PARTS 2 CNX_VER_PATCH)
add_compile_definitions(
  CNX_VERSION_STR="${CNX_VERSION}"
  CNX_VER_MAJOR=${CNX_VER_MAJOR}
  CNX_VER_MINOR=${CNX_VER_MINOR}
  CNX_VER_PATCH=${CNX_VER_PATCH})

include_directories(dsp shared)

add_executable(maximizer_test tests/maximizer_test.cpp)
add_executable(equalizer_test tests/equalizer_test.cpp)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apo/CompressorApo.cpp")
  add_library(CompressorNextApo SHARED
    apo/CompressorApo.cpp
    apo/dllmain.cpp
    apo/CompressorNextApo.def
    apo/CompressorNextApo.rc)
  target_link_libraries(CompressorNextApo PRIVATE
    AudioBaseProcessingObjectV140.lib audiomediatypecrt.lib audioeng.lib ole32.lib advapi32.lib uuid.lib)

  add_executable(apo_host_test tests/apo_host_test.cpp)
  target_link_libraries(apo_host_test PRIVATE audiomediatypecrt.lib ole32.lib uuid.lib)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/svc/main.cpp")
  add_executable(CompressorNextSvc
    svc/main.cpp
    svc/Binding.cpp
    svc/Verify.cpp
    svc/CompressorNextSvc.rc)
  target_link_libraries(CompressorNextSvc PRIVATE advapi32.lib ole32.lib)
endif()
