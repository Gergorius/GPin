rec {
	memory.pkgs = {
		url = "https://github.com/NixOS/nixpkgs";
		rev = "d5cc6833999746cffdfe0598f7bc801a57b887a9";
		shallow = true;
		update.rev = (builtins.fromJSON (builtins.readFile (builtins.fetchurl { url = "https://api.github.com/repos/NixOS/nixpkgs/commits/nixos-unstable"; }))).sha;
	};

	# We are stuck developing against an old version because the linker is failing for reasons unknown. The version differences are starting to build up. This will be a pain to maintain.

	memory.pkgs-stable = {
		url = "https://github.com/NixOS/nixpkgs";
		rev = "f4f698677b11021a8f84f452e23ae9ef2427bec3";
		shallow = true;
#		update.rev = (builtins.fromJSON (builtins.readFile (builtins.fetchurl { url = "https://api.github.com/repos/NixOS/nixpkgs/commits/nixos-26.05"; }))).sha;
	};
	pkgs = import (builtins.fetchGit { inherit (memory.pkgs) url rev shallow; }) {};
	pkgs-stable = import (builtins.fetchGit { inherit (memory.pkgs-stable) url rev shallow; }) {};
}