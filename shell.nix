{
	pkgs ? import <nixpkgs> {},
}:
	pkgs.mkShell {
		packages = with pkgs; [
			nix
			nix.src
			clang-tools
		];
		inputsFrom = [
			(pkgs.callPackage ./build.nix {})
		];
	}