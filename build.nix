{ pkgs, stdenv, lib, ... }:
let
	fs = lib.fileset;
	der = pkgs.clangStdenv.mkDerivation {
		name = "pun";
		src = fs.toSource {
			root = ./.;
			fileset = fs.unions [ ./src ./CMakeLists.txt ];
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