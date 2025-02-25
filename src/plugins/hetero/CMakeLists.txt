# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#

set (TARGET_NAME "ov_hetero_plugin")

file(GLOB SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)

ie_add_plugin(NAME ${TARGET_NAME}
              DEVICE_NAME "HETERO"
              SOURCES ${SOURCES} ${HEADERS}
              VERSION_DEFINES_FOR plugin.cpp
              ADD_CLANG_FORMAT)

ie_faster_build(${TARGET_NAME}
    UNITY
)

target_link_libraries(${TARGET_NAME} PRIVATE pugixml::static)

ie_add_api_validator_post_build_step(TARGET ${TARGET_NAME})

set_target_properties(${TARGET_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ${ENABLE_LTO})
