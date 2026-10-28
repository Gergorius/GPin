let
	pins = import ./pins.nix;

	build = { pkgs ? pins.pkgs }:
		let {
			inherit (pkgs) stdenv lib;
			
			fs = lib.fileset;

			gpin-derivation = pkgs.clangStdenv.mkDerivation {
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

			body = {
				gpin-update = gpin-derivation;
				gpin = pkgs.callPackage ./gpin body;
			};
		};
in
	build