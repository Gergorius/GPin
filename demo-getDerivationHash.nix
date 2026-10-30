let
	pkgs = import <nixpkgs> {};
in rec {

	# Demonstrating getDerivationHash's usage. Remember that the intended use is PINNING the hash.

	# This is a bad fixed-output derivation because it's output depends on the current time. There is no way we know the hash in advance.

	bad-derivation = pkgs.stdenv.mkDerivation {
		name = "baba";
		nativeBuildInputs = [ pkgs.git pkgs.cacert ];
		unpackPhase = "true";
		buildPhase = "date +%s%3N > $out";
		outputHashAlgo = "sha256";
		outputHashMode = "recursive";
		outputHash = "";
	};

	# I introduce you to the doctor. He fills in the correct hash. Don't question his practices or his license. Or the fact getDerivationHash is not a pure function.

	doctor = patient: patient.overrideAttrs { outputHash = builtins.gpin.getDerivationHash patient; };

	cured-derivation = doctor bad-derivation;

	# Now the derivation builds and we can read from it. It's like magic!

	baba = "1793388355130\n";
	update.baba = builtins.readFile "${cured-derivation}";
}
