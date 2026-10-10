let
	pins = import ./pins.nix;
in {
	pkgs ? pins.pkgs,
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