
# This file contains several tests for whether subexpressions are correctly enumerated by gpin-update.

{

	verify = {old, new}: assert builtins.all (x: x) (builtins.attrValues (builtins.mapAttrs (name: body: body new.${name}) old.check)); "expr successfully updated";

	assert_cond = assert { baba = false; update.baba = true; }.baba; "success";
	check.assert_cond = v: v == "success";

	assert_body = assert ([5] == [5]); { baba = a: a; update.baba = null; };
	check.assert_body = v: v == {baba = null; update.baba = null;};

	with_body = with { keke = a: a; update.keke = 15; }; { keke = keke; };
	check.with_body = v: v.keke == 15;

	let_attrs = let fofo = { rock = 5; update.rock = 10; }; in fofo.rock * 3;
	check.let_attrs = v: v == 30;

	let_update = let rock = 5; update.rock = 10; in rock * 3;
	check.let_update = v: v == 15; # we do not update attributes in let directly

	let_attrs_update = let fofo.rock = 5; fofo.update.rock = 10; in fofo.rock * 3;
	check.let_attrs_update = v: v == 30; # syntax does not matter

	let_body = let flag = "win"; in { wall = 5; update.wall = flag; };
	check.let_body = v: v == { wall = "win"; update.wall = "win"; };

	if_cond = if { power = false; update.power = true; }.power then "powered" else "unpowered";
	check.if_cond = v: v == "powered";

	if_branch_true = if { power = false; update.power = true; }.power then { rule = "move"; update.rule = "push"; } else "unpowered";
	check.if_branch_true = v: v == {rule = "push"; update.rule = "push"; };

	if_branch_false = if { power = true; update.power = false; }.power then "powered" else { rule = "float"; update.rule = "sink"; };
	check.if_branch_false = v: v == {rule = "sink"; update.rule = "sink"; };

	app_left = (rec { coeff = 1; update.coeff = 2; fun = v: v * coeff; }).fun 5;
	check.app_left = v: v == 10;

	app_right = (x: x.jiji * 5) ({ jiji = 5; update.jiji = 15; });
	check.app_right = v: v == 75;

	attr_path_base = { water = null; update.water = "wet"; }.water;
	check.attr_path_base = v: v == "wet";

	attr_path_dynamic = { a = 1; b = 2; }.${{ name = "a"; update.name = "b";}.name};
	check.attr_path_dynamic = v: v == 2;

	bracketed = (((((((rec { meta = "ba"; update.meta = meta + meta; })))))));
	check.bracketed = v: v == { meta = "baba"; update.meta = "babababa"; };

	let_binds = let { body = -7; update.body = 3; };
	check.let_binds = v: v == 3;

	dynamic_binding = { ${{ name = "a"; update.name = "b"; }.name} = "c"; };
	check.dynamic_binding = v: v == {b = "c";};

	dynamic_binding_value = { ${{ name = "a"; update.name = "b"; }.name} = { bird = "fly"; update.bird = "float";}; };
	check.dynamic_binding_value = v: v == {b.bird = "float"; b.update.bird = "float";};

	inherit_from = { inherit ({ level = "float"; update.level = "move"; }) level; };
	check.inherit_from = v: v == {level = "move";};

	string_part = "baba is ${{verb = "stop"; update.verb = "you";}.verb}";
	check.string_part = v: v == "baba is you";

	not = !({ cliff = false; update.cliff = true; }.cliff);
	check.not = v: v == false;

	neg = -({ cliff = 5; update.cliff = 10; }.cliff);
	check.neg = v: v == -10;
}