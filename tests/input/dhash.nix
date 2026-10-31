
# nix-build is called on this with the shared object's path as argument.

{ sharedObject }:
let
	gpin = builtins.importNative sharedObject "gpin_lib";

	input = {
		name = "test";
		builder = "/bin/sh";
		system = builtins.currentSystem;
		args = ["-c" "read -r x < /proc/uptime && printf '%s\n' \"$x\" > $out"];
		outputHashAlgo = "sha256";
		outputHash = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
	};

	# So far, this derivation cannot work because it is a fixed output derivation with an intentionally variable output.

	baba = derivation input;
	hash = gpin.getDerivationHash baba;
	keke = derivation (input // {outputHash = hash; });

	# But our function "getDerivationHash" does it's magic!
in
	assert builtins.isString hash; keke