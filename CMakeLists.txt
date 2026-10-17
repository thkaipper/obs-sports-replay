cmake_minimum_required(VERSION 3.28...3.30)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(SR_USE_EXISTING_SDK "Use an explicit prebuilt OBS SDK via CMAKE_PREFIX_PATH" OFF)
if(NOT ENABLE_QT OR NOT ENABLE_FRONTEND_API)
  message(FATAL_ERROR "Sports Replay requires Qt and the OBS frontend API")
endif()

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(FFmpeg REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::avformat)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/sr-buffer.c
    src/sr-codec.c
    src/capture-filter.c
    src/playback-source.c
    src/sr-scene-tracker.c
    src/sr-clip.c
    src/sr-save.cpp
    src/sr-integration.cpp
    src/sr-load.c
    src/sr-config.c
    src/sr-thumb.c
    src/sr-dock.cpp
)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX)

option(SR_BUILD_TESTS "Build isolated libobs integration tests" OFF)
if(SR_BUILD_TESTS)
  enable_testing()
  add_executable(sr-integration-test tests/integration-host.cpp)
  target_compile_features(sr-integration-test PRIVATE cxx_std_17)
  target_compile_definitions(sr-integration-test PRIVATE NOMINMAX)
  target_include_directories(sr-integration-test PRIVATE src)
  target_link_libraries(
    sr-integration-test
    PRIVATE OBS::libobs Qt6::Core Qt6::Widgets FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
  )
  add_executable(
    sr-media-test
    tests/media-unit.cpp
    src/sr-buffer.c
    src/sr-codec.c
    src/sr-load.c
    src/sr-clip.c
    src/sr-save.cpp
  )
  target_compile_features(sr-media-test PRIVATE cxx_std_17)
  target_compile_definitions(sr-media-test PRIVATE NOMINMAX)
  target_include_directories(sr-media-test PRIVATE src)
  target_link_libraries(
    sr-media-test
    PRIVATE plugin-support OBS::libobs Qt6::Core FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale
  )

  add_executable(
    sr-playback-test
    tests/playback-regression.c
    src/sr-buffer.c
    src/sr-codec.c
    src/sr-load.c
    src/sr-clip.c
  )
  add_executable(sr-capture-test tests/capture-regression.c src/sr-buffer.c)
  add_executable(
    sr-initialization-test
    tests/initialization-regression.cpp
    src/capture-filter.c
    src/sr-buffer.c
    src/sr-codec.c
    src/sr-save.cpp
  )
  add_executable(sr-dock-test tests/dock-regression.cpp src/sr-scene-tracker.c)
  foreach(test_target sr-playback-test sr-capture-test sr-initialization-test sr-dock-test)
    target_compile_features(${test_target} PRIVATE cxx_std_17)
    target_compile_definitions(${test_target} PRIVATE NOMINMAX)
    target_include_directories(${test_target} PRIVATE src)
    target_link_libraries(
      ${test_target}
      PRIVATE
        plugin-support
        OBS::libobs
        OBS::obs-frontend-api
        Qt6::Core
        Qt6::Widgets
        FFmpeg::avformat
        FFmpeg::avcodec
        FFmpeg::avutil
        FFmpeg::swscale
    )
  endforeach()

  set(SR_OBS_RUNTIME_DIR "C:/Program Files/obs-studio" CACHE PATH "OBS installation used by isolated tests")
  foreach(
    case
    media
    integration
    capture
    initialization
    dock
  )
    if(case STREQUAL "media")
      set(case_target sr-media-test)
    else()
      set(case_target sr-${case}-test)
    endif()
    add_test(
      NAME sr-${case}
      COMMAND
        ${CMAKE_COMMAND} "-DCASE=${case}" "-DEXE=$<TARGET_FILE:${case_target}>"
        "-DPLAYBACK=$<TARGET_FILE:sr-playback-test>" "-DDLL=$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
        "-DDATA=${CMAKE_CURRENT_SOURCE_DIR}/data" "-DRESULTS=${CMAKE_CURRENT_BINARY_DIR}/test-results/$<CONFIG>" -P
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-case.cmake"
    )
    set_tests_properties(sr-${case} PROPERTIES TIMEOUT 120)
    if(WIN32)
      set_tests_properties(
        sr-${case}
        PROPERTIES
          ENVIRONMENT "SR_OBS_RUNTIME_DIR=${SR_OBS_RUNTIME_DIR};QT_PLUGIN_PATH=${SR_OBS_RUNTIME_DIR}/bin/64bit"
          ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SR_OBS_RUNTIME_DIR}/bin/64bit"
      )
    endif()
  endforeach()
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
