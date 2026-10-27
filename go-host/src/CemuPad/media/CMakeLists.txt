add_library(CemuPadH264 STATIC H264.cpp RtpH264.cpp VideoFrames.cpp AudioFrames.cpp)
target_compile_features(CemuPadH264 PUBLIC cxx_std_20)
target_include_directories(CemuPadH264 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../..)
set_target_properties(CemuPadH264 PROPERTIES POSITION_INDEPENDENT_CODE ON)
