cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()
project(dolphinblue_vita C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Dolphin Blue")
set(VITA_TITLEID  "DLPB00001")
set(VITA_VERSION  "01.00")
set(CMAKE_CXX_STANDARD 17)
set(R ${CMAKE_SOURCE_DIR}/..)
# -O3 like the released Vita ports (Rinnegatamante). DB_PGO=gen: instrumented build that writes its profile to
# ux0:data/dolphinblue/pgo (autotest "pgodump N"); DB_PGO=use: optimise with the profile copied to DB_PGO_DIR.
set(DB_PGO "off" CACHE STRING "profile-guided optimisation: off|gen|use")
set(DB_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "profile directory for DB_PGO=use")
set(COMMON_FLAGS "-O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-strict-aliasing -Wno-tautological-compare")
if(DB_PGO STREQUAL "gen")
  set(COMMON_FLAGS "${COMMON_FLAGS} -fprofile-generate=ux0:data/dolphinblue/pgo -fprofile-update=single -DDB_PGO_GEN=1")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-generate")
elseif(DB_PGO STREQUAL "use")
  set(COMMON_FLAGS "${COMMON_FLAGS} -fprofile-use=${DB_PGO_DIR} -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch")
endif()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS}")

file(GLOB GEN ${R}/gen/db_funcs_*.cpp)
add_library(db_recomp STATIC ${GEN} ${R}/gen/db_dispatch.cpp)
target_include_directories(db_recomp PUBLIC ${R}/runtime ${R}/gen)

set(HOST ${R}/host/mem.cpp ${R}/host/sh4p.cpp ${R}/host/holly.cpp ${R}/host/aica.cpp ${R}/host/aica_mix.cpp ${R}/host/aica_dsp.cpp ${R}/host/native_vtx.cpp ${R}/host/arm7.cpp
         ${R}/host/bios.cpp ${R}/host/savestate.cpp ${R}/host/debug.cpp ${R}/render/ta_decode.cpp ${R}/render/pvr_tex.cpp
         ${CMAKE_SOURCE_DIR}/vita_main.cpp ${CMAKE_SOURCE_DIR}/gxm_backend.cpp)
add_executable(dolphinblue ${HOST})
target_include_directories(dolphinblue PRIVATE ${R}/runtime ${R}/host ${R}/render ${CMAKE_SOURCE_DIR}/third_party/libperf)
target_compile_definitions(dolphinblue PRIVATE DB_VITA=1)
target_compile_options(dolphinblue PRIVATE -include ${CMAKE_SOURCE_DIR}/vita_log.h)
target_link_libraries(dolphinblue db_recomp vitashark SceShaccCgExt taihen_stub SceShaccCg_stub_weak
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceSysmodule_stub SceKernelDmacMgr_stub SceAppMgr_stub
    SceAppUtil_stub SceCommonDialog_stub SceIofilemgr_stub ScePower_stub SceLibKernel_stub SceKernelThreadMgr_stub SceRtc_stub SceAudio_stub ${CMAKE_SOURCE_DIR}/third_party/libperf/lib/libScePerf_stub_weak.a m stdc++)
if(DB_PGO STREQUAL "gen")
  target_sources(dolphinblue PRIVATE ${CMAKE_SOURCE_DIR}/pgo_stubs.cpp)   # the value profilers vitasdk's libgcov lacks
  set_source_files_properties(${CMAKE_SOURCE_DIR}/pgo_stubs.cpp PROPERTIES COMPILE_OPTIONS "-fno-profile-arcs;-fno-profile-values")
  target_link_libraries(dolphinblue gcov)   # the profiling runtime (__gcov_dump, the counters' writer)
endif()
vita_create_self(eboot.bin dolphinblue UNSAFE)
# offline shader bank (vita/shaders/*.gxp, generated once with the "shaderbank" switch) -> app0:shaders
file(GLOB SHADER_BANK ${CMAKE_SOURCE_DIR}/shaders/*.gxp)
# LiveArea images (sce_sys/icon0.png, sce_sys/livearea/contents/{bg,startup}.png, template.xml) are packed when present
set(VPK_FILES FILE ${CMAKE_SOURCE_DIR}/third_party/libperf/lib/libperf.suprx libperf.suprx FILE ${CMAKE_SOURCE_DIR}/third_party/libperf/lib/libperf.skprx libperf.skprx
    FILE ${CMAKE_SOURCE_DIR}/third_party/libperf/LICENSE licenses/libperf-LICENSE.txt FILE ${CMAKE_SOURCE_DIR}/../../THIRD-PARTY-NOTICES.md licenses/THIRD-PARTY-NOTICES.md)
foreach(a icon0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml)
  if(EXISTS ${CMAKE_SOURCE_DIR}/sce_sys/${a})
    list(APPEND VPK_FILES FILE ${CMAKE_SOURCE_DIR}/sce_sys/${a} sce_sys/${a})
  endif()
endforeach()
foreach(g ${SHADER_BANK})
  get_filename_component(n ${g} NAME)
  list(APPEND VPK_FILES FILE ${g} shaders/${n})
endforeach()
vita_create_vpk(dolphinblue.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME} ${VPK_FILES})
