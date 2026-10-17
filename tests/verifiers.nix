{
	trivial-verify = {old, new, testname, ...}:
		assert old // old.update == new; "${testname} successfully updated";
	check-verify = {old, new, testname, ...}:
		let
			all_checks = builtins.mapAttrs (name: body: assert body new.${name}; true) old.check;
		in
			builtins.deepSeq all_checks "${testname} successfully updated";
}