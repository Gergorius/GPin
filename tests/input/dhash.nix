
# nix-build is called on this with the shared object's path as argument.

{ sharedObject }:
let
	hpat = ".*-([0-9a-zA-Z=+/]{3,}).*";

	gpin = builtins.importNative sharedObject "gpin_lib";

	input = {
		name = "dhash";
		builder = "/bin/sh";
		system = builtins.currentSystem;
		args = ["-c" "read -r x < /proc/uptime && printf '%s\n' \"$x\" > $out"];
		outputHashAlgo = "sha256";
		outputHash = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
	};

	# So far, this derivation cannot work because it is a fixed output derivation with an intentionally variable output.

	baba = derivation input;
	result = gpin.getErrorMessage (builtins.readFile baba);
	hash = builtins.head (builtins.match hpat result);
	keke = derivation (input // { outputHash = hash; });

	# We can fix it by trying to build it anyway and then extracting the correct hash from the error message.
in
	assert builtins.isString hash; keke