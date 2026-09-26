cmake_minimum_required(VERSION 3.23)

find_package(PkgConfig REQUIRED)

project(Pun)

add_executable(pun)

target_compile_features(pun PRIVATE cxx_std_23)

pkg_check_modules(nix REQUIRED IMPORTED_TARGET nix-expr nix-cmd)

target_link_libraries(pun PRIVATE PkgConfig::nix)

target_sources(pun
  PRIVATE
	altparse.cpp
	format.cpp
	greedy.cpp
	treewalk.cpp
	cthunk.cpp
	pun.cpp
)

install(TARGETS pun DESTINATION ${CMAKE_INSTALL_BINDIR})