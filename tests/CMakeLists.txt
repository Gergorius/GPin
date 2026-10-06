cmake_minimum_required(VERSION 3.23)

set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)

function(makeTrivialTest name file)
	file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}/${name}")
	add_test(
		NAME ${name}
		WORKING_DIRECTORY "${TEST_OUTPUT_DIR}/${name}"
		COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/testers/trivial.sh" $<TARGET_FILE:gpin-update> ${file})
endfunction()

set(INPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/input)

makeTrivialTest(trivial "${INPUT_DIR}/trivial.nix")
makeTrivialTest(attrs "${INPUT_DIR}/attrs.nix")