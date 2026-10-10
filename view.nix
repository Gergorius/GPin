let
	pins = import ./pins.nix;
in {
	pkgs ? pins.pkgs,
}:
	pkgs.linkFarm "dependencies" {
		nix = pkgs.nix;
		nix-dev = pkgs.nix.dev;
		nix-src = pkgs.nix.src;
		nix-expr = pkgs.nix.libs.nix-expr;
		nix-expr-c = pkgs.nix.libs.nix-expr-c;
	}