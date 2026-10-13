# nsscore: the .nss config library for Nilesoft Shell Settings.
# Builds standalone (cmake -S src/settings/nss) or via add_subdirectory.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(nsscore LANGUAGES CXX)
	set(NSSCORE_STANDALONE ON)
else()
	set(NSSCORE_STANDALONE OFF)
endif()

option(NSSCORE_BUILD_TESTS "Build the nsscore tests" ${NSSCORE_STANDALONE})

add_library(nsscore STATIC
	src/Text.cpp
	src/NssLexer.cpp
	src/Literal.cpp
	src/NssDocument.cpp
	src/ThemeSchema.cpp
	src/ConfigGraph.cpp
	src/OverridesWriter.cpp
	src/ConfigStore.cpp
	src/Json.cpp
	src/Json.h
	include/nss/nss.h
	include/nss/Text.h
	include/nss/NssLexer.h
	include/nss/Literal.h
	include/nss/NssDocument.h
	include/nss/ThemeSchema.h
	include/nss/ConfigGraph.h
	include/nss/OverridesWriter.h
	include/nss/ConfigStore.h
)
add_library(nss::core ALIAS nsscore)

target_include_directories(nsscore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(nsscore PUBLIC cxx_std_20)
target_compile_definitions(nsscore PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(nsscore PUBLIC bcrypt)
if(MSVC)
	target_compile_options(nsscore PRIVATE /W4 /permissive- /utf-8)
endif()

if(NSSCORE_BUILD_TESTS)
	enable_testing()

	add_executable(nsscore_tests
		tests/main.cpp
		tests/TestUtil.h
		tests/test_text.cpp
		tests/test_lexer.cpp
		tests/test_document.cpp
		tests/test_schema.cpp
		tests/test_graph.cpp
		tests/test_overrides.cpp
		tests/test_store.cpp
	)
	target_include_directories(nsscore_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)
	target_link_libraries(nsscore_tests PRIVATE nsscore)
	target_compile_definitions(nsscore_tests PRIVATE
		UNICODE _UNICODE NOMINMAX
		NSS_SOURCE_BIN="${CMAKE_CURRENT_SOURCE_DIR}/../../bin"
	)
	if(MSVC)
		target_compile_options(nsscore_tests PRIVATE /W4 /permissive- /utf-8)
	endif()

	add_test(NAME nsscore_tests COMMAND nsscore_tests)
endif()
