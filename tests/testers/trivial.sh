set -e

PROGRAM="$1"
SOURCE="$2"

export NIX_STATE_DIR=$(pwd)/nix-state
export NIX_LOG_DIR=$(pwd)/nix-log

TARGET=$(basename $SOURCE)
cp $SOURCE $TARGET

$PROGRAM $TARGET

nix-instantiate --eval --strict --raw --arg old "import $SOURCE" --arg new "import ./$TARGET" - <<EOF
{old, new}: assert (old // old.update == new); "$TARGET successfully updated"
EOF
