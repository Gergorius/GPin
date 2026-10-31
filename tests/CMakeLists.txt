cmake_minimum_required(VERSION 3.23)

set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)

function(makeTrivialTest name file)
	file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}/${name}")
	get_filename_component(file_name ${file} NAME)
	set(test_file "${TEST_OUTPUT_DIR}/${name}/${file_name}")
	
	# Step 1: Make a copy of the file.
	add_test(NAME "${name}/setup" COMMAND ${CMAKE_COMMAND} -E copy "${file}" "${TEST_OUTPUT_DIR}/${name}/${file_name}")
	set_tests_properties("${name}/setup" PROPERTIES
		FIXTURES_SETUP "setup_fixture_${name}"
	)

	# Step 2: Run the program on the file.
	add_test(NAME "${name}/run" COMMAND $<TARGET_FILE:gpin-update> ${test_file} --extra-experimental-features "pipe-operators")
	set_tests_properties("${name}/run" PROPERTIES
		FIXTURES_REQUIRED "setup_fixture_${name}"
		FIXTURES_SETUP "run_fixture_${name}"
		ENVIRONMENT "NIX_STATE_DIR=${TEST_OUTPUT_DIR}/${name}/nix-state;NIX_LOG_DIR=${TEST_OUTPUT_DIR}/${name}/nix-log"
	)

	# Step 3: Verify with nix-instantiate.
	add_test(NAME "${name}/verify"
		COMMAND
			nix-instantiate --eval --strict --raw
			--arg old "import ${file}"
			--arg new "import ${test_file}"
			--argstr testname ${name}
			--expr "{old, ...}@args: old.verify args"
			--extra-experimental-features "pipe-operators"
			--show-trace
		)
	set_tests_properties("${name}/verify" PROPERTIES
		FIXTURES_REQUIRED "run_fixture_${name}"
		ENVIRONMENT "NIX_STATE_DIR=${TEST_OUTPUT_DIR}/${name}/nix-state;NIX_LOG_DIR=${TEST_OUTPUT_DIR}/${name}/nix-log"
	)
endfunction()

set(INPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/input)

makeTrivialTest(trivial "${INPUT_DIR}/trivial.nix")
makeTrivialTest(attrs "${INPUT_DIR}/attrs.nix")
makeTrivialTest(expr "${INPUT_DIR}/expr.nix")
makeTrivialTest(binary "${INPUT_DIR}/binary.nix")
makeTrivialTest(cycle "${INPUT_DIR}/cycle.nix")

file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}/dhash")
add_test(NAME "dhash"
	COMMAND
		nix-build
		--store ${TEST_OUTPUT_DIR}/dhash/nix-store
		--option build-dir ${TEST_OUTPUT_DIR}/dhash/nix-build
		"${INPUT_DIR}/dhash.nix"
		--arg sharedObject "$<TARGET_FILE:gpin-natives>"
		--allow-unsafe-native-code-during-evaluation --no-out-link
)
set_tests_properties("dhash" PROPERTIES
	ENVIRONMENT "NIX_STATE_DIR=${TEST_OUTPUT_DIR}/dhash/nix-state;NIX_LOG_DIR=${TEST_OUTPUT_DIR}/dhash/nix-log"
)