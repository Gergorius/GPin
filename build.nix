{ pkgs ? (import ./pins.nix).pkgs, lib ? pkgs.lib }:
let
	fs = lib.fileset;

	gpin-update = pkgs.clangStdenv.mkDerivation {
		name = "gpin";
		src = fs.toSource {
			root = ./.;
			fileset = fs.unions [ ./src ./CMakeLists.txt ./tests ];
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
		doCheck = true;
	};
in {
	inherit gpin-update;
}