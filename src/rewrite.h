
#include "format.h"
#include "valueutil.h"
#include <nix/expr/attr-set.hh>
#include <nix/expr/value.hh>
#include <unordered_map>

namespace gpin{

struct ValueStatistics{
	// Weight of the value. This is a huge value if it is cyclic.
	int32_t weight;
};

// Context-independent cache for private use by the rewriter. Should be relatively short lived.
struct RewriteCache{
	NixValueInternPool internPool;
	NixValueTopology components;
	std::unordered_map<const nix::Value*,ValueStatistics> valueStatistics;
	RewriteCache(nix::EvalState& eval) : internPool(eval){}
	const ValueStatistics& lookupStatistics(const nix::Value* value);
};

struct RewriteState : FormatState{
	RewriteCache& cache;
	inline RewriteState(FormatState&& fs,RewriteCache& c): FormatState(std::move(fs)), cache(c){}
};

template<typename TargetBindings>
void generateRewrites(RewriteState& state, TargetBindings& target, const nix::Bindings& bindings, bool writeExisting, bool writeNew);

}