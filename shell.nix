let
	pins = import ./pins.nix;
in {
	pkgs ? pins.pkgs-stable,
}:
	pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
		packages = with pkgs; [
			nix
			nix.src
			clang-tools
			lldb
		];
		inputsFrom = [
			(pkgs.callPackage ./build.nix { inherit pkgs; }).gpin-update
		];
	}