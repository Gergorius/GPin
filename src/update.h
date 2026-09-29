
#include "altparse.h"
#include "greedy.h"
#include "exprutil.h"
#include "format.h"

#include <boost/container_hash/hash.hpp>
#include <boost/unordered/concurrent_flat_map_fwd.hpp>
#include <memory>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <nix/main/shared.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/cmd/common-eval-args.hh>
#include <gc/gc_allocator.h>

#include <nix/util/pos-idx.hh>
#include <nix/util/pos-table.hh>
#include <nix/util/source-path.hh>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

using std::shared_ptr;

struct RecordedExprAttrs;

struct SourceInfo{
	ParseResult parseResult;
	std::shared_ptr<const std::string> content;
	nix::RootValue value;
	nix::SourcePath basePath;
	SourceInfo(nix::SourcePath bp): basePath(bp){};
};

struct EvalStateForUpdate : EvalState{
	using EvalState::EvalState;

	nix::Symbol updateSymbol;

	// A list of values that must be forceValue'd.
	std::vector<nix::Value*, traceable_allocator<nix::Value*>> forceQueue;
	
	std::unordered_map<RecordedExprAttrs*, std::shared_ptr<nix::Value* const>> valueMap;
	std::unordered_map<nix::SourcePath, SourceInfo> fileSources;
	std::list<SourceInfo> customSources;
	inline virtual ~EvalStateForUpdate(){} // Ensure this is polymorphic. (EvalState isn't unfortunately!)

	const SourceInfo& loadFile(const nix::SourcePath& path);
	// The stdin argument determines whether to treat the input string as sourced from stdin.
	const SourceInfo& loadString(nix::SourcePath basePath, std::string data, bool isStdin);

	// Call this after all load operations are performed. This evaluates all loaded attribute sets.
	void finishLoad();

	void doRewrite(std::ostream& out, const SourceInfo& sourceInfo);
};

struct RecordedExprAttrs : nix::ExprAttrs{
	RecordedExprAttrs(nix::ExprAttrs&& t): nix::ExprAttrs(std::move(t)){}
	virtual void eval(EvalState &state, Env &env, Value &v) override;
	virtual Value* maybeThunk(EvalState& state, Env& env) override;
};

struct ObtrusiveVisitor{
	void subexpr(nix::Expr* e) const{
		visitDynamicExpr(e, *this);
	}
	void operator()(nix::Expr*) const{
		// TODO: We should error here.
	}
	void operator()(nix::ExprFloat*) const{}
	void operator()(nix::ExprInt*) const{}
	void operator()(nix::ExprString*) const{}
	void operator()(nix::ExprVar*) const{}
	void operator()(nix::ExprPath*) const{}
	void operator()(nix::ExprInheritFrom*) const{}
	void operator()(nix::ExprBlackHole*) const{}
	// Lambda's subexpressions are left alone.
	void operator()(nix::ExprLambda* expr) const{
		obtrudeAsGreedy<nix::ExprLambda>(expr);
	}
	void operator()(nix::ExprAttrs* expr) const{
		visitSubexprs(expr,[&](nix::Expr* expr){ this->subexpr(expr); });
		obtrudeAsGreedy<nix::ExprAttrs,RecordedExprAttrs>(expr);
	}
	template<typename T>
	void operator()(T* expr) const{
		visitSubexprs(expr, [&](nix::Expr* expr){ this->subexpr(expr); });
		obtrudeAsGreedy<T>(expr);
	}
};

namespace{
	inline SyntaxReference& descendHelper(RecordedExprAttrs* a, SyntaxReference& sr){
		return sr;
	}
	inline SyntaxReference descendHelper(RecordedExprAttrs* a, RawSyntaxReference& sr){
		return sr.descendToIsolated(a);
	}
}
