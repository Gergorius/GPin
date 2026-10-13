
# Binary operators are so numerous they get their separate test.

{
	verify = {old, new, ...}: assert builtins.all (x: x) (builtins.attrValues (builtins.mapAttrs (name: body: body new.${name}) old.check)); "binary successfully updated";

	eq = {noun = "wall"; update.noun = "baba";} == {noun = "rock"; update.noun = "baba";};
	check.eq = v: v == true;

	neq = {noun = "cliff"; update.noun = "keke";} != {noun = "wall"; update.noun = "keke";};
	check.neq = v: v == false;

	lt = {grass = 10; update.grass = 8;}.grass < {foliage = 7; update.foliage = 9;}.foliage;
	check.lt = v: v == true;

	le = {dog = 15; update.dog = 14;}.dog <= {cat = 13; update.cat = 14;}.cat;
	check.le = v: v == true;

	gt = {rose = 3; update.rose = 5;}.rose > {violet = 6;update.violet = 4;}.violet;
	check.gt = v: v == true;

	ge = {crab = 5; update.crab = 6;}.crab >= {star = 7;update.star = 6;}.star;
	check.ge = v: v == true;

	and = {rock = false; update.rock = true;}.rock && {box = false; update.box = true;}.box;
	check.and = v: v == true;

	or = {gem = true; update.gem = false;}.gem || {cog = true; update.cog = false;}.cog;
	check.or = v: v == false;

	impl = {ufo = false; update.ufo = true;}.ufo -> {rocket = true; update.rocket = false;}.rocket;
	check.impl = v: v == false;

	intrs = {fence = 5.5; update.fence = 6.5;} // {hedge = 7.5; update.hedge = 7.25;};
	check.intrs = v: v == { fence = 6.5; hedge = 7.25; update.hedge = 7.25;};

	has_attr_left = {flag = {}; update.flag.win = true;}.flag ? win;
	check.has_attr_left = v: v == true;

	has_attr_right = {a = 0;} ? ${{name = "b"; update.name = "a";}.name};
	check.has_attr_right = v: v == true;

	add = {fofo = false;update.fofo = 1;}.fofo + {jiji = false;update.jiji = 1;}.jiji;
	check.add = v: v == 2;

	sub = {arrow = false;update.arrow = 100;}.arrow - {eye = false;update.eye = 5;}.eye;
	check.sub = v: v == 95;

	times = {road = false;update.road = 7;}.road * {track = false;update.track = 3;}.track;
	check.times = v: v == 21;

	div = {door = false;update.door = 200;}.door / {key = false;update.key = 25;}.key;
	check.div = v: v == 8;

	concat = {text = null; update.text = [0 1 2 3];}.text ++ {write = null; update.write = [4 5 6 7];}.write;
	check.concat = v: v == [0 1 2 3 4 5 6 7];
}