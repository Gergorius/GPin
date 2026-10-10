let
	pins = import ./pins.nix;
in {
	pkgs ? pins.pkgs,
}:
	pkgs.callPackage ./build.nix {}