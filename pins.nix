rec {
	memory.pkgs = {
		url = "https://github.com/NixOS/nixpkgs";
		rev = "7fd4004d0511991fff3a0524856a9be09e28c5f0";
		shallow = true;
		update.rev = (builtins.fromJSON (builtins.readFile (builtins.fetchurl { url = "https://api.github.com/repos/NixOS/nixpkgs/commits/nixos-unstable"; }))).sha;
	};
	memory.pkgs-stable = {
		url = "https://github.com/NixOS/nixpkgs";
		rev = "f4f698677b11021a8f84f452e23ae9ef2427bec3";
		shallow = true;
#		update.rev = (builtins.fromJSON (builtins.readFile (builtins.fetchurl { url = "https://api.github.com/repos/NixOS/nixpkgs/commits/nixos-26.05"; }))).sha;
	};
	pkgs = import (builtins.fetchGit { inherit (memory.pkgs) url rev shallow; }) {};
	pkgs-stable = import (builtins.fetchGit { inherit (memory.pkgs-stable) url rev shallow; }) {};
}