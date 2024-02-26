cmake_minimum_required(VERSION 3.25)

project(headless)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(Vulkan REQUIRED COMPONENTS glslc volk)

set(SHADERS_IN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADERS_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(GLOB SHADERS "${SHADERS_IN_DIR}/*.vert" "${SHADERS_IN_DIR}/*.frag" "${SHADERS_IN_DIR}/*.comp")
file(MAKE_DIRECTORY ${SHADERS_OUT_DIR})

foreach(SHADER ${SHADERS})
	get_filename_component(SHADER_NAME ${SHADER} NAME)
	set(SHADER_OUT_NAME "${SHADERS_OUT_DIR}/${SHADER_NAME}.spv")
	list(APPEND SHADER_OUT_NAMES ${SHADER_OUT_NAME})
	add_custom_command(
		MAIN_DEPENDENCY ${SHADER}
		OUTPUT ${SHADER_OUT_NAME}
		COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SHADER} "-o" ${SHADER_OUT_NAME}
		VERBATIM)
endforeach()

add_custom_target(build_shaders DEPENDS ${SHADER_OUT_NAMES})

add_executable(headless main.cpp videoencoder.cpp)
target_include_directories(headless PRIVATE $<TARGET_PROPERTY:Vulkan::volk,INTERFACE_INCLUDE_DIRECTORIES>)
add_dependencies(headless build_shaders)
