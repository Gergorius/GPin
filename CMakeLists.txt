cmake_minimum_required(VERSION 3.23)

find_package(PkgConfig REQUIRED)

project(GPin)

add_compile_options(-fstandalone-debug)

add_executable(gpin-update)

target_compile_features(gpin-update PRIVATE cxx_std_23)

pkg_check_modules(nix REQUIRED IMPORTED_TARGET nix-expr nix-cmd)

target_link_libraries(gpin-update PRIVATE PkgConfig::nix)

target_sources(gpin-update
	PRIVATE
		src/altparse.cpp
		src/format.cpp
		src/greedy.cpp
		src/treewalk.cpp
		src/primitives.cpp
		src/update.cpp
		src/valueutil.cpp
		src/main.cpp
)

install(TARGETS gpin-update DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()
add_subdirectory(tests)