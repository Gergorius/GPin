{ pkgs, stdenv, lib, ... }:
let
	fs = lib.fileset;
	der = stdenv.mkDerivation {
		name = "pun";
		src = fs.toSource {
			root = ./src;
			fileset = ./src;
		};
		nativeBuildInputs = with pkgs; [
			clang
			nix.dev
			pkg-config
		];
		buildInputs = with pkgs.nix.libs; [
			nix-expr
			nix-cmd
		];
		buildPhase = "clang++ $src/main.cpp $(pkg-config --libs --cflags nix-expr nix-cmd) -o $out";
	};
in
	der