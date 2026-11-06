
# nix-build is called on this with the shared object's path as argument.

{ sharedObject }:
let
	gpin = builtins.importNative sharedObject "gpin_lib";

	input = { name = "buildlog"; builder = "/bin/sh"; system = builtins.currentSystem; args = ["-c" "echo baba is you; exit 1"]; };

	baba = derivation input;
in
	assert gpin.tryEval (builtins.readFile baba) == false;
	assert gpin.getBuildLog baba.drvPath == "baba is you\n";
	baba
