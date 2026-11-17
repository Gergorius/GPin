{ pkgs ? (import ./pins.nix).pkgs, lib ? pkgs.lib }:
let
	fs = lib.fileset;
in pkgs.clangStdenv.mkDerivation (finalAttrs: {
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
	passthru.natives = pkgs.writeTextFile {
		name = "gpin-natives.nix";
		text = "builtins.importNative ${finalAttrs.finalPackage.out}/bin/libgpin-natives.so \"gpin_lib\"";
	};
})