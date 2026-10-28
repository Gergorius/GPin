let
	vfers = import ../verifiers.nix;
	ceq = vfers.cycle-safe-eq;
in rec {
	verify = vfers.check-verify;

	remove-rec = {
		target = rec {};
		target.keke = 5;
		target = rec { baba = 5; };
		update.target = { baba = 7; keke = 7; };
	};
	check.remove-rec = v: remove-rec // remove-rec.update == v;

	add-rec = {
		target = { baba = 3; };
		update.target = rec { keke = { fofo = keke; }; };
	};
	check.add-rec = v: ceq add-rec.update.target v.target;

	add-rec-declaration = {
		target.baba = 5;
		target.keke = 7;
		update.target = rec { baba = 10; keke = { fofo = keke; };};
	};
	check.add-rec-declaration = v: ceq add-rec-declaration.update.target v.target;

	add-rec-declaration-2 = {
		target.baba = 5;
		target.keke = 7;
		update.target = rec { baba = 10; keke = { fofo = keke; }; jiji = keke; };
	};
	check.add-rec-declaration-2 = v: ceq add-rec-declaration-2.update.target v.target;

	simple-cse = {
		target = null;
		update.target =
			let
				baba = [keke 5 keke];
				keke = {fofo = [baba keke.fofo]; jiji = [1 2 3 4];};
			in
				[keke 5 keke];
	};
	check.simple-cse = v: ceq simple-cse.update.target v.target;

	list-cse = {
		target = null;
		update.target =
			let
				# Numbers are necessary because "builtins.elem" stack overflows. Must be the spaghetti.
				a = [0 f f];
				b = [1 a a];
				c = [2 b b];
				d = [3 c c];
				e = [4 d d];
				f = [5 e e];
			in
				a;
	};
	check.list-cse = v: ceq list-cse.update.target v.target;

	attr-cse = {
		target = null;
		update.target =
			let
				# I invite the reader to check out what this gets turned into.
				a = {inherit a b c d e f;};
				b = {inherit a;};
				c = {inherit a b;};
				d = {inherit a b c;};
				e = {inherit a b c d;};
				f = {inherit a b c d e;};
			in
				{inherit b;};
	};
	check.attr-cse = v: ceq attr-cse.update.target v.target;
}