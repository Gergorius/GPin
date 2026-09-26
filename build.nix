{ pkgs, stdenv, lib, ... }:
let
	fs = lib.fileset;
	der = pkgs.clangStdenv.mkDerivation {
		name = "pun";
		src = fs.toSource {
			root = ./src;
			fileset = ./src;
		};
		nativeBuildInputs = with pkgs; [
			cmake
			nix.dev
			pkg-config
		];
		buildInputs = with pkgs.nix.libs; [
			nix-expr
			nix-cmd
		];
	};
in
	der