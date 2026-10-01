
# Run gpin-update on this to see how it works.

rec {
	# Example 1: Update to the latest revision.

	channel = "a7bc1acfc84719a6c3219f9e13b0df3d6d0091fa";
	update.channel = (builtins.fetchGit {
		url = "https://github.com/NixOS/nix";
		ref = "master";
	}).rev;

	# Example 2: Increment a version number.

	version = 1;
	update.version = version + 1;

	# Example 3: Log the update history of another value.

	history = [];
	update.history = history ++ [ channel ];

	# Example 4: Note the time of the latest update.

	lastUpdated = 1788693742;
	update.lastUpdated = builtins.currentTime;

	# Example 5: Caching the result of an IFD (Import From Derivation). Useful if the value is known to only change when the pinned source does and is short.

	stringJSONContents = ["" "hi" "white rabbit" "大白兔"];
	update.stringJSONContents =
		let
			nix = builtins.fetchGit {
				url = "https://github.com/NixOS/nix";
				rev = update.channel; # We reference the fresh update directly. We do not want to lag behind it after all.
			};
		in
			builtins.fromJSON (builtins.readFile "${nix}/src/libstore-tests/data/serve-protocol/string.json");

	# Example 6: Self overwriting update.

	# "update" is itself a valid target for updating.

	containmentUnit = {
		update = {update = {update = {update = "Goodbye";};};};
	};

	# Example 7: Mask function.

	# Partially applied primitive operations are preserved.

	maskFunction = 0;
	update.maskFunction = builtins.bitAnd version;
}
