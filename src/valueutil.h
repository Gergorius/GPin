
#include <nix/expr/eval.hh>
#include <nix/expr/value.hh>
#include <nix/util/pos-idx.hh>

// Hash function for nix values.
struct NixValueHash{
	size_t operator()(nix::Value* vptr) const;
};

// Deep equality for nix values.
struct NixValueEq{
	bool operator()(nix::Value* l,nix::Value* r) const;
};