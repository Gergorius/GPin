{
	pkgs ? import <nixpkgs> {},
}:
	pkgs.mkShell {
		packages = with pkgs; [
			nix
			nix.src
			clang-tools
			lldb
		];
		inputsFrom = [
			(pkgs.callPackage ./build.nix {})
		];
	}