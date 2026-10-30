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

	mixed-cse-0 = rec {
		target = null;
		update.target = let
			v0 = {v1 = v1;};
			v1 = [v0];
			in
				v0;
	};
	check.mixed-cse-0 = v: ceq mixed-cse-0.update.target v.target;

	mixed-cse-1 = rec {
		target = null;
		update.target = let
			v0 = {v1 = v1;};
			v1 = [v2 v2];
			v2 = [v0];
			in
				v0;
	};
	check.mixed-cse-1 = v: ceq mixed-cse-1.update.target v.target;

	mixed-cse-2 = {
		target = null;
		update.target = let
			v0 = {v1 = v1;};
			v1 = [v0];
			in
				v0;
	};
	check.mixed-cse-2 = v: ceq mixed-cse-2.update.target v.target;

	mixed-cse-3 = {
		target = null;
		update.target = let
			v0 = {v1 = v1;};
			v1 = [v2 v2];
			v2 = [v0];
			in
				v0;
	};
	check.mixed-cse-3 = v: ceq mixed-cse-3.update.target v.target;

	mixed-cse-4 = rec {
		target0 = null;
		target1 = null;
		update =
			let
				target0 = {v0 = v0;};
				target1 = {self = target1; v1 = v1;};
				v0 = [v1 v1];
				v1 = [v0];
			in {
				inherit target0 target1;
			};
	};
	check.mixed-cse-4 = v: ceq mixed-cse-4.update.target0 v.target0 && ceq mixed-cse-4.update.target1 v.target1;

	mixed-cse-5 = rec {
		target0 = null;
		target1 = null;
		update =
			let
				target0 = {a = v0;};
				v0 = [v1 v1];
				v1 = [v0];
				target1 = {b = v0;};
			in {
				inherit target0 target1;
			};
	};
	check.mixed-cse-5 = v: ceq mixed-cse-5.update.target0 v.target0 && ceq mixed-cse-5.update.target1 v.target1;
}