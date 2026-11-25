# PrintMiiOut core: the model pipeline in C++, with assimp reading the GLBs
# and writing the STL. Builds a command line tool natively and a
# WebAssembly module with Emscripten (see core/README.md).
cmake_minimum_required(VERSION 3.20)
project(printmiiout_core C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

option(PMO_SYSTEM_ASSIMP "Use an installed assimp instead of building one" OFF)

include(FetchContent)

if(EMSCRIPTEN)
	# assimp reports import errors with exceptions.
	add_compile_options(-fwasm-exceptions)
	add_link_options(-fwasm-exceptions)
endif()

if(PMO_SYSTEM_ASSIMP)
	find_package(assimp REQUIRED)
else()
	# Only what the pipeline needs: the glTF importer.
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
	set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
	set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
	set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
	set(ASSIMP_IGNORE_GIT_HASH ON CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_DRACO OFF CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
	set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
	FetchContent_Declare(assimp
		GIT_REPOSITORY https://github.com/assimp/assimp.git
		GIT_TAG v6.0.6
		GIT_SHALLOW TRUE)
	FetchContent_MakeAvailable(assimp)
	# Not our code: keep its warnings out of the build log. Its bundled
	# poly2tri leans on <ostream> arriving through other headers, which
	# newer libc++ (Emscripten's) no longer does.
	target_compile_options(assimp PRIVATE -w $<$<COMPILE_LANGUAGE:CXX>:-include ostream>)
	if(TARGET zlibstatic)
		target_compile_options(zlibstatic PRIVATE -w)
	endif()
endif()

add_library(pmo_core STATIC
	src/mesh_ops.cpp
	src/image.cpp
	src/body.cpp
	src/head.cpp
	src/fuse.cpp
	src/simplify.cpp
	src/split.cpp
	src/raster.cpp
	src/face2d.cpp
	src/text2d.cpp
	src/sdf.cpp
	src/trinkets.cpp
	src/flexi.cpp
	src/pipeline.cpp)
target_include_directories(pmo_core PUBLIC src third_party)
target_link_libraries(pmo_core PUBLIC assimp::assimp)
# zlib deflates the multi-material 3MF: the copy assimp builds, or the system's.
if(TARGET zlibstatic)
	target_link_libraries(pmo_core PRIVATE zlibstatic)
	target_include_directories(pmo_core PRIVATE ${assimp_SOURCE_DIR}/contrib/zlib ${assimp_BINARY_DIR}/contrib/zlib)
else()
	find_package(ZLIB REQUIRED)
	target_link_libraries(pmo_core PRIVATE ZLIB::ZLIB)
endif()

if(EMSCRIPTEN)
	add_executable(printmiiout-core src/wasm_api.cpp)
	target_link_libraries(printmiiout-core PRIVATE pmo_core)
	target_link_options(printmiiout-core PRIVATE
		-lembind
		-sMODULARIZE=1
		-sEXPORT_ES6=1
		-sEXPORT_NAME=createPrintMiiOut
		-sENVIRONMENT=web,worker,node
		-sALLOW_MEMORY_GROWTH=1
		-sINITIAL_MEMORY=64MB
		-sMAXIMUM_MEMORY=4GB
		-sSTACK_SIZE=2MB
		-sFILESYSTEM=0)
	# The site serves the module from static/client, next to its worker.
	set(PMO_CLIENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../static/client" CACHE PATH "Where the built module goes")
	add_custom_command(TARGET printmiiout-core POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy
			$<TARGET_FILE_DIR:printmiiout-core>/printmiiout-core.js
			$<TARGET_FILE_DIR:printmiiout-core>/printmiiout-core.wasm
			${PMO_CLIENT_DIR}
		COMMENT "Copying the module to ${PMO_CLIENT_DIR}")
else()
	add_executable(printmiiout-core src/cli.cpp)
	target_link_libraries(printmiiout-core PRIVATE pmo_core)
endif()
