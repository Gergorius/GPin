
# Each nix test file contains a "verify" attribute which the tester evaluates. It normally returns a simple string such as "x successfully verified" as a confirmation that it ran. Note that the tester calls the OLD file's verify with the contents of both the OLD and NEW file.

rec {
	attrNames = set: builtins.sort (a: b: (builtins.unsafeGetAttrPos a set).line < (builtins.unsafeGetAttrPos b set).line) (builtins.attrNames set);

	trivial-verify = {old, new, testname, ...}:
		assert old // old.update == new; "${testname}: ${builtins.concatStringsSep ", " (attrNames old)}";
	
	check-verify = {old, new, testname, ...}:
		let
			all_checks = builtins.mapAttrs (
				name: body: builtins.addErrorContext "while running checker '${name}'" (assert body new.${name}; null)
			) old.check;
		in
			builtins.deepSeq all_checks "${testname}: ${builtins.concatStringsSep ", " (attrNames old.check)}";
	
	cycle-safe-eq = l: r:
		let
			list-helper = s: l: r:
				if builtins.length l == builtins.length r then
					(builtins.foldl' ({i,s}: _: {i = i+1; s = helper s (builtins.elemAt l i) (builtins.elemAt r i);}) {i=0;s=s;} l).s
				else
					null;
			helper = s: l: r:
				if s == null then
					null
				else if builtins.elem [l r] s then
					s
				else let
					lt = builtins.typeOf l;
					rt = builtins.typeOf r;
					s2 = [[l r]] ++ s;
				in if lt != rt then
					null
				else {
					list = list-helper s2 l r;
					set = if builtins.attrNames l == builtins.attrNames r then list-helper s2 (builtins.attrValues l) (builtins.attrValues r) else null;
				}.${lt} or (if [l] == [r] then s2 else null);
		in
			(helper [] l r) != null;
	
	test-cycle-safe-eq =
		assert  cycle-safe-eq [] [];
		assert  cycle-safe-eq [0] [0];
		assert  cycle-safe-eq [0 1] [0 1];
		assert !cycle-safe-eq [0 1 2] [0 1 2 3];
		assert !cycle-safe-eq [0 1 2] [0 1 3];
		assert !cycle-safe-eq [0 1 2] [0 3 2];
		assert !cycle-safe-eq [0 1 2] [3 1 2];
		assert  cycle-safe-eq {} {};
		assert  cycle-safe-eq {baba=5;} {baba=5;};
		assert !cycle-safe-eq {baba=5;} {keke=5;};
		assert !cycle-safe-eq {baba=5;} {baba=0;};
		assert  cycle-safe-eq {baba=[0 1 2];} {baba=[0 1 2];};
		assert !cycle-safe-eq {baba=[0 1 2];} {baba=[0 1 3];};
		assert  cycle-safe-eq rec {keke.baba = keke;} rec {keke.baba = keke;};
		assert  cycle-safe-eq rec {keke.baba = keke;keke.fofo = 5;} rec {keke.baba = keke;keke.fofo = 5;};
		assert !cycle-safe-eq rec {keke.baba = keke;keke.fofo = 5;} rec {keke.baba = keke;keke.fofo = 6;};
		assert !cycle-safe-eq rec {keke.fofo = keke;keke.baba = 5;} rec {keke.fofo = keke;keke.baba = 6;};
		assert  cycle-safe-eq rec {keke.fofo = keke;keke.baba = keke;} rec {keke.fofo = keke;keke.baba = keke;}; # This is why we do not use depth cap.
		true;
}