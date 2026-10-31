cmake_minimum_required(VERSION 3.23)

find_package(PkgConfig REQUIRED)

pkg_check_modules(nix-expr REQUIRED IMPORTED_TARGET nix-expr)
pkg_check_modules(nix-cmd REQUIRED IMPORTED_TARGET nix-cmd)

project(GPin)

set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_BINDIR}")

if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
	add_compile_options(-fstandalone-debug)
endif()

add_library(gpin-natives
	SHARED
		src/primitives.cpp
)

add_executable(gpin-update)

target_compile_features(gpin-update PRIVATE cxx_std_26)
target_compile_features(gpin-natives PRIVATE cxx_std_26)

target_link_libraries(gpin-update PRIVATE PkgConfig::nix-expr)
target_link_libraries(gpin-update PRIVATE PkgConfig::nix-cmd)
target_link_libraries(gpin-natives PRIVATE PkgConfig::nix-expr)

target_link_libraries(gpin-update PRIVATE gpin-natives)

target_sources(gpin-update
	PRIVATE
		src/altparse.cpp
		src/format.cpp
		src/greedy.cpp
		src/treewalk.cpp
		src/update.cpp
		src/valueutil.cpp
		src/rewrite.cpp
		src/main.cpp
)

install(TARGETS gpin-update DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS gpin-natives DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()
add_subdirectory(tests)