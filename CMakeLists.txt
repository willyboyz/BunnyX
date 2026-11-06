cmake_minimum_required(VERSION 3.18)
project(nyushu_framegen CXX)
set(CMAKE_CXX_STANDARD 17)

# glslc ikut di dalam NDK (shader-tools). Bisa dioverride: -DGLSLC=/path/glslc
if(NOT GLSLC)
  file(GLOB _g "${ANDROID_NDK}/shader-tools/*/glslc" "${ANDROID_NDK}/shader-tools/*/glslc.exe")
  list(LENGTH _g _n)
  if(_n EQUAL 0)
    message(FATAL_ERROR "glslc tidak ketemu di NDK. Pakai -DGLSLC=/path/ke/glslc")
  endif()
  list(GET _g 0 GLSLC)
endif()

set(INCS "")
foreach(s luma motion interp)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/${s}.inc
    COMMAND ${GLSLC} -mfmt=num -fshader-stage=compute --target-env=vulkan1.0 -O
            ${CMAKE_SOURCE_DIR}/shaders/${s}.comp -o ${CMAKE_BINARY_DIR}/${s}.inc
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${s}.comp)
  list(APPEND INCS ${CMAKE_BINARY_DIR}/${s}.inc)
endforeach()

add_library(VkLayer_khronos_validation SHARED src/layer.cpp ${INCS})
target_include_directories(VkLayer_khronos_validation PRIVATE ${CMAKE_BINARY_DIR})
target_compile_options(VkLayer_khronos_validation PRIVATE -O2 -fvisibility=hidden)
target_link_options(VkLayer_khronos_validation PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)
target_link_libraries(VkLayer_khronos_validation PRIVATE log)
