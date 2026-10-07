{
	verify = {old, new}: assert old // old.update == new; "trivial successfully updated";
	update.verify = null;

	bool = false;
	update.bool = true;

	str = "baba";
	update.str = "keke";

	int = 0;
	update.int = 15;

	float = 0.0;
	update.float = 1.5;

	attrs = {};
	update.attrs = {flag = "win";};

	attrs.bool = false;
	update.attrs.bool = true;

	list = [];
	update.list = [
		0
		9
		2.7
		{ wall = "stop"; }
		[ 0 1 2 3 ]
		"rock"
		false
		null
		true
		(-5)
		(-2.8)
		(1.e308*3)
		(-1.e308*3)
	];

	doNotChange = null;
}