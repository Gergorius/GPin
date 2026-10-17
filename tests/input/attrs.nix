
# This file contains several tests for how gpin-update rewrites weird attribute sets.

let
	x = "you";
	t = { a = 0; b = 1; c = 2; };
in {
	verify = (import ../verifiers.nix).trivial-verify;
	update.verify = null;

	baba = { inherit x; };
	update.baba.y = 10;

	keke = {};
	keke = { inherit x; };
	update.keke.y = 15;

	fofo = {};
	fofo = { inherit (t) a b c; };
	update.fofo.y = 20;

	rock = {};
	rock = { inherit (t) a b c; };
	update.rock.a = 25;
	update.rock.b = 30;
	update.rock.d = 35;

	wall = { inherit (t) a b c; };
	wall = {};
	update.wall.a = 25;
	update.wall.b = 30;
	update.wall.d = 35;

	flag = {};
	flag.a = 0;
	flag.b = 100;
	flag.c = 200;

	update.flag = { inherit (t) a b c; };
	update.flag.d = 3;

	robot.a = 43;
	update.robot.b = 44;

	cog = null;
	update.cog.this.is.a.really.deep.attribute = {};

	cloud = { ${x} = 7; };
	update.cloud = {};

	cliff.${x} = 8;
	update.cliff = { you = 9; };

	box = {
		a = -1;
		b = -2;
		c = -3;
		d = -4;
	};

	update.box = {
		a = 2;
		b = 4;
		c = 6;
		e = 8;
	};

	water = {
		${x} = true;
		is = false;
	};
	water.has = "box";
	update.water = null;

	lava = {
		dust.defeat = 5;
		dust.${x} = 55;
		bird.float = false;
	};
	update.lava = {
		dust.defeat = 15;
	};
	update.lava = {
		bird.float = true;
		bird.sink = false;
	};

	foliage.defeat = 5;
	foliage.push = 6;
	foliage.pull = 7;
	foliage.more = 8;
	foliage = {};
	foliage = {};
	update.foliage = "empty";
}