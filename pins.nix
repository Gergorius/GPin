rec {
	memory.pkgs = {
		url = "https://github.com/NixOS/nixpkgs";
		rev = "e7439b6b14ad3cc35d05608ebca9bce01a25f5f8";
		shallow = true;
		update.rev = (builtins.fromJSON (builtins.readFile (builtins.fetchurl { url = "https://api.github.com/repos/NixOS/nixpkgs/commits/nixos-unstable"; }))).sha;
	};
	pkgs = import (builtins.fetchGit { inherit (memory.pkgs) url rev shallow; }) {};
}