
#include "rewrite.h"
#include "format.h"
#include "valueutil.h"
#include <cmath>
#include <format>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <nix/util/error.hh>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wimplicit-fallthrough"

namespace gpin{

using nix::Value;

struct BasicChartValueInfo{
	const BasicChartValueInfo* pathParent = nullptr;
	nix::Symbol pathName = nix::Symbol();
	BasicChartValueInfo(){}
};

static const BasicChartValueInfo root_store = BasicChartValueInfo();
static const BasicChartValueInfo* path_unreachable = nullptr;
static const BasicChartValueInfo* path_env = &root_store;

bool samePath(const BasicChartValueInfo* left, const BasicChartValueInfo* right){
	if(left == right){
		return true;
	}
	if(left == path_env || right == path_env || left == path_unreachable || right == path_unreachable){
		return false;
	}
	return left->pathName == right->pathName && samePath(left->pathParent, right->pathParent);
}

struct ChartValueInfo : BasicChartValueInfo{
	uint32_t pathCost = 0xFFFFFFFFu;
	ChartValueInfo(){}
};

struct ChartContext{
	FormatState& state;
	std::unordered_map<const Value*, ChartValueInfo>& paths;
};

static inline void clampAdd(uint32_t& a,uint32_t b,uint32_t c = 0){
	uint64_t sum = ((uint64_t)a) + ((uint64_t)b) + ((uint64_t)c);
	a = (uint32_t)std::min(sum,(uint64_t)0xFFFFFFFFu);
}

uint32_t chartWeight(nix::EvalState& state, const Value* value, std::unordered_map<const nix::Value*,ValueStatistics>& statistics){
	static constexpr uint32_t maxWeight = 0xFFFFFFFFu;

	auto [itr, inserted] = statistics.insert({value,ValueStatistics{}});
	ValueStatistics& stat = itr->second;
	if(!inserted){
		return stat.weight;
	}
	stat.weight = maxWeight;
	uint32_t totalWeight = 0;

	switch(internalType(*value)){
		case nix::tListN:
		case nix::tListSmall:
			totalWeight = 2;
			for(nix::Value* vptr : value->listView()){
				clampAdd(totalWeight,chartWeight(state, vptr, statistics));
			}
			break;
		case nix::tAttrs:
			totalWeight = 2;
			for(const nix::Attr& attr : *value->attrs()){
				clampAdd(totalWeight, chartWeight(state, attr.value, statistics), 2 + state.symbols[attr.name].size());
				if(totalWeight == maxWeight) break;
			}
			break;
		case nix::tPrimOpApp:{
			totalWeight = 1;
			clampAdd(totalWeight, chartWeight(state, value->primOpApp().left, statistics));
			if(totalWeight == maxWeight) break;
			clampAdd(totalWeight, chartWeight(state, value->primOpApp().right, statistics));
		}break;
		case nix::tApp:{
			totalWeight = 1;
			clampAdd(totalWeight, chartWeight(state, value->app().left, statistics));
			if(totalWeight == maxWeight) break;
			clampAdd(totalWeight, chartWeight(state, value->app().right, statistics));
		}break;
		case nix::tBool:
			totalWeight = value->boolean() ? 4 : 5;
			break;
		case nix::tUninitialized:
		case nix::tNull:
			totalWeight = 4;
			break;
		case nix::tString:
			totalWeight = value->string_data().size();
			break;
		default:
			totalWeight = 5; // No more complex logic yet.
			break;
	}

	stat.weight = totalWeight;

	return totalWeight;
};

const ValueStatistics& RewriteCache::lookupStatistics(const nix::Value* value){
	auto itr = valueStatistics.find(value);
	if(itr == valueStatistics.end()){
		chartWeight(internPool.state, value, valueStatistics);
		itr = valueStatistics.find(value);
	}
	return itr->second;
}

static void chart(ChartContext& context, const Value* value, ChartValueInfo& info){
	if(value->type() != nix::nAttrs){
		return;
	}
	for(auto& bnd : *value->attrs()){
		const nix::Value* subValue = bnd.value;

		ChartValueInfo& subInfo = context.paths[subValue];

		uint32_t subPathCost = info.pathCost + context.state.eval().symbols[bnd.name].size() + 1;

		if(subPathCost < subInfo.pathCost){
			subInfo.pathParent = &info;
			subInfo.pathCost = subPathCost;
			subInfo.pathName = bnd.name;
			chart(context, subValue, subInfo);
		}
	}
}

// Reverse environment used for substituting values.
struct FormatEnv{
	friend const ChartValueInfo* lookup(const FormatEnv* env, const Value* vptr, bool direct);
	friend bool wasMadeRecursive(const FormatEnv* env, const Value* vptr);
	const Value* definedForValue;
private:
	using map_type = std::unordered_map<nix::Symbol, std::pair<const Value*,std::unordered_map<const Value*, ChartValueInfo>>>;
	map_type data;
	const FormatEnv* parent;
	struct Inhibitor{
		const Inhibitor* parent;
		const map_type& map;
	};
	void lookupBestReference(const Value* vptr, const Inhibitor* inhibitor, const ChartValueInfo*& bestMatch, bool direct = false) const{
		for(auto& [sym, map] : data){
			auto itr = map.second.find(vptr);
			if(itr == map.second.end()){
				continue;
			}
			const ChartValueInfo* info = &itr->second;
			const Inhibitor* li = inhibitor;
			while(li != nullptr){
				if(li->map.contains(sym)){
					goto continueOuter;
				}
				li = li->parent;
			}
			if(bestMatch == nullptr || info->pathCost < bestMatch->pathCost){
				bestMatch = info;
			}
		continueOuter:;
		}
		if(!direct && parent != nullptr){
			Inhibitor ti{.parent = inhibitor, .map = data};
			parent->lookupBestReference(vptr, &ti, bestMatch);
		}
	}
public:
	FormatEnv(): parent(nullptr){}
	FormatEnv(const FormatEnv* p): parent(p){}
	bool containsSymbol(nix::Symbol symbol) const{
		return data.contains(symbol);
	}
	bool addSymbol(FormatState& state, nix::Symbol symbol, const Value* value){
		auto itr = data.insert({symbol, {value, {}}});
		std::unordered_map<const Value*, ChartValueInfo>& paths = itr.first->second.second;
		if(itr.second){
			string_view symbolName = state.eval().symbols[symbol];

			// The first symbol must be a var name, otherwise we won't be able to reference it. The rest of the symbols can be whatever.
			if(isVarName(symbolName)){
				ChartValueInfo& i = paths.insert({value, ChartValueInfo{}}).first->second;
				i.pathParent = path_env;
				i.pathName = symbol;
				i.pathCost = symbolName.size();
				ChartContext context = ChartContext{
					.state = state,
					.paths = paths
				};
				chart(context, value, i);
			}
		}
		return !paths.empty();
	}
};

// Was this value already made recursive?
bool wasMadeRecursive(const FormatEnv* env, const Value* vptr){
	while(env != nullptr){
		if(env->definedForValue == vptr){
			return true;
		}
		env = env->parent;
	}
	return false;
}

// Find the path to a value. Returns path_unreachable if the value is not reachable. If the direct argument is true, parent environments are ignored.
const ChartValueInfo* lookup(const FormatEnv* env, const Value* vptr, bool direct = false){
	const ChartValueInfo* ref = nullptr;
	if(env != nullptr){
		env->lookupBestReference(vptr, nullptr, ref, direct);
	}
	return ref;
}

#define STREAM(os,code) [&]() -> std::string{ std::ostringstream os; code; return std::move(os).str(); }()

static void formatPath(FormatState& rs, Cursor state, const BasicChartValueInfo* path){
	while(path != path_env){
		Cursor temp;
		if(path->pathParent != path_env){
			temp = state.split();
			state << ".";
		}
		state << STREAM(os,nix::printAttributeName(os, rs.eval().symbols[path->pathName]));
		state = std::move(temp);
		path = path->pathParent;
	}
};

static bool possiblyFormatLookup(FormatState& rs, const BasicChartValueInfo* path, const FormatEnv* env, Cursor& state, const Value& value){
	const BasicChartValueInfo* lookupPath = lookup(env, &value);
	if(lookupPath != path_unreachable && !samePath(lookupPath, path)){
		formatPath(rs, std::move(state), lookupPath);
		return true;
	}else{
		return false;
	}
}

static nix::Symbol generateSymbol(nix::SymbolTable& table){
	uint32_t id = 0;
	std::string s;
	while(true){
		s.clear();
		s += "rec";
		s += std::to_string(id);
		
		// SymbolTable provides no way to tell whether a symbol exists. We want to add a new symbol so we check the before-after size to test if we succeeded. The reason we want to add a new symbol is to ensure it will result in unique variables that can not be hidden. By the point this method is called, the value tree is fully evaluated.

		uint32_t preSize = table.totalSize();
		nix::Symbol newSymbol = table.create(s);
		uint32_t postSize = table.totalSize();

		if(preSize != postSize){
			return newSymbol;
		}
		id++;
	}
}


static bool isPApp(const nix::Value& v){
	return v.isApp() || v.isPrimOpApp();
}

static nix::Value::FunctionApplicationThunk getPApp(const nix::Value& v){
	if(v.isApp()){
		return v.app();
	}else{
		return nix::Value::FunctionApplicationThunk{.left = v.primOpApp().left, .right = v.primOpApp().right};
	}
}

/*

Plan 2:

- When seeing an isolated attrs, perform an "explore" up to non-attr values. (Stop when seeing a non-attr value!)
	- Explore counts, per value, how many values point to it.
	- If there is a value of decent weight with many pointers, rec.
- If the attrs is not isolated, do nothing.

- When seeing a list/app series, perform an "explore" up to attr values. (Stop at attrs!)
	- Explore should count, per value, how many values point to it.
	- Every value with decent weight that has more than one pointers is lifted to let.

*/

static constexpr uint32_t CSE_WEIGHT_THRESHOLD = 50;

// @abbrev CSE: Common Subexpression Elimination

template<typename Target>
static void formatValue(RewriteState& state, const BasicChartValueInfo* path, const FormatEnv* env, Target target, const Value& value, bool skipCSE){

#define CS(prec) Cursor cs = std::move(target).toSimple(state,prec).toCursor()
	const nix::InternalType vType = internalType(value);
	switch(vType){

		case nix::tBool:{
			CS(precedence::simple);
			if(value.boolean()){
				cs << "true";
			}else{
				cs << "false";
			}
		}break;

		case nix::tString:{
			CS(precedence::simple);
			if(value.context()){
				Value temp = value;
				state.eval().forceStringNoCtx(temp, nix::noPos, "while trying to update an attribute");
			}
			const nix::ExprString* existingString = dynamic_cast<nix::ExprString*>(target.getExpression());
			cs << STREAM(os,nix::printLiteralString(os, value.string_view()));
		}break;

		case nix::tNull:{
			CS(precedence::simple);
			cs << "null";
		}break;

		case nix::tPath:{
			CS(precedence::simple);
			nix::SourcePath sp = value.path();
			if(sp.accessor == state.basePath.accessor){
				const std::string& base = state.basePath.path.abs();
				const std::string& subp = sp.path.abs();
				if(subp.starts_with(base)){
					cs << "." << std::move(subp).substr(base.size());
				}else{
					cs << sp.to_string();
				}
			}else{
				cs << sp.to_string();
			}
		}break;

		case nix::tInt:{
			CS(0 <= value.integer() ? precedence::simple : precedence::op);
			cs << std::to_string(value.integer().value);
		}break;
		
		case nix::tFloat:{
			// Why is this so complicated?
			auto f = value.fpoint();

			if(std::isnan(f)){
				CS(precedence::op);
				cs << "1.e308*2*0";
			}else if(std::isinf(f)){
				CS(precedence::op);
				if(std::signbit(f)){
					cs << "-1.e308*2";
				}else{
					cs << "1.e308*2";
				}
			}else{
				CS(0.0 <= f ? precedence::simple : precedence::op);
				cs << std::format("{:-#}", f);
			}
		}break;
		
		case nix::tAttrs:
		case nix::tListN:
		case nix::tListSmall:
		case nix::tPrimOpApp:
		case nix::tApp:{

			{
				uint32_t size = compoundValueSize(&value);
				
				if(size == 0){
					CS(precedence::simple);
					if(vType == nix::tAttrs){
						cs << "{}";
					}else{
						cs << "[]";
					}
					break;
				}
			}

			{
				const BasicChartValueInfo* lookupPath = lookup(env, &value);
				if(lookupPath != path_unreachable && !samePath(lookupPath,path)){
					CS(precedence::simple);
					formatPath(state, std::move(cs), lookupPath);
					break;
				}
			}

			if(!skipCSE && !wasMadeRecursive(env, &value)){
				bool wantRecursive = false;
				{
					bool useLet = false;
					struct enc_info{
						uint32_t count = 0;
						bool reachable = false;
						enc_info(){}
					};
					std::unordered_map<const nix::Value*, enc_info> encounters;

					[&](this const auto& recurse, const Value* vptr, bool reachable, bool count){
						enc_info& info = encounters[vptr];
						if(count){
							info.count++;
							count = info.count == 1;
						}
						bool reachableChanged = !info.reachable && reachable;
						if(reachable){
							info.reachable = true;
							reachable = vptr->type() == nix::nAttrs;
						}
						if(count || reachableChanged && reachable){
							visitSubvalues(vptr, recurse, reachable, count);
						}
						return false;
					}(&value, true, true);
					
					if(encounters[&value].count != 1){
						useLet = wantRecursive = true;
					}

					auto itr = encounters.begin();
					while(itr != encounters.end()){
						if(2 <= itr->second.count && CSE_WEIGHT_THRESHOLD <= state.cache.lookupStatistics(itr->first).weight){
							wantRecursive = true;
							useLet = useLet || !itr->second.reachable;
							itr++;
						}else{
							itr = encounters.erase(itr);
						}
					}

					if(useLet){
						struct value_field_info{
							const nix::Value* vptr;
							uint32_t topology;
							nix::Symbol symbol;
						};
						std::vector<value_field_info> cseVector;
						cseVector.reserve(encounters.size());
						for(auto mapping : encounters){
							cseVector.push_back({
								.vptr = mapping.first,
								.topology = state.cache.components.lookupTopologyIndex(mapping.first)
							});
						}
						std::sort(cseVector.begin(),cseVector.end(),[](auto left,auto right){ return left.topology < right.topology; });

						FormatEnv newEnv = FormatEnv(env);
						{
							auto read = cseVector.begin();
							auto write = read;
							while(read != cseVector.end()){
								if(lookup(&newEnv, read->vptr) == path_unreachable){
									read->symbol = generateSymbol(state.eval().symbols);
									newEnv.addSymbol(state, read->symbol, read->vptr);
									*write = *read;
									write++;
								}
								read++;
							}
							cseVector.resize(std::distance(cseVector.begin(),write));
						}
						std::visit([&](auto letTarget){
							for(auto pair : cseVector){
								BasicChartValueInfo pt{};
								pt.pathName = pair.symbol;
								pt.pathParent = path_env;
								std::visit(
									[&](auto letAttr){
										formatValue(state, &pt, &newEnv, std::move(letAttr), *pair.vptr, true);
									},
									letTarget.makeStaticAttribute(state, pair.symbol)
								);
							}
							formatValue(state, path_unreachable, &newEnv, std::move(letTarget).finish(state), value, true);
						}, std::move(target).toLet(state));

						return;
					}
				}

				if(wantRecursive){
					FormatEnv localEnv(env);

					for(auto binding : *value.attrs()){
						localEnv.addSymbol(state, binding.name, binding.value);
					}

					std::visit([&](auto bindingsTarget){
						for(auto binding : *value.attrs()){
							BasicChartValueInfo pt;
							pt.pathParent = path_env;
							pt.pathName = binding.name;
							std::visit([&](auto bindingExpr){
								formatValue(state, &pt, &localEnv, std::move(bindingExpr), *binding.value, true);
							}, bindingsTarget.makeStaticAttribute(state, binding.name));
						}
						std::move(bindingsTarget).finish(state);
					}, std::move(target).toBindings(state, true));

					return;
				}
			}

			switch(vType){
				case nix::tAttrs:
					if(path == path_unreachable){
						std::visit([&](auto bindingsTarget){
							for(auto binding : *value.attrs()){
								std::visit([&](auto bindingExpr){
									formatValue(state, path_unreachable, env, std::move(bindingExpr), *binding.value, true);
								}, bindingsTarget.makeStaticAttribute(state, binding.name));
							}
							std::move(bindingsTarget).finish(state);
						}, std::move(target).toBindings(state, false));
					}else{
						std::visit([&](auto bindingsTarget){
							for(auto binding : *value.attrs()){
								BasicChartValueInfo pt;
								pt.pathParent = path;
								pt.pathName = binding.name;
								std::visit([&](auto bindingExpr){
									formatValue(state, &pt, env, std::move(bindingExpr), *binding.value, true);
								}, bindingsTarget.makeStaticAttribute(state, binding.name));
							}
							std::move(bindingsTarget).finish(state);
						}, std::move(target).toBindings(state, false));
					}
					break;
				case nix::tListN:
				case nix::tListSmall:
					std::visit([&](auto listTarget){
						for(uint32_t i = 0;i < value.listSize();i++){
							std::visit([&](auto valueExpr){
								formatValue(state, path_unreachable, env, std::move(valueExpr), *value.listView()[i], true);
							},listTarget.nextExpr());
						}
						std::move(listTarget).finish(state);
					}, std::move(target).toList(state));
					break;
				case nix::tPrimOpApp:
				case nix::tApp:
					std::visit([&](auto appPair){
						formatValue(state, path, env, std::move(appPair.first), *getPApp(value).left, true);
						formatValue(state, path, env, std::move(appPair.second), *getPApp(value).right, true);
					}, std::move(target).toApp(state));
					break;
				default:
					std::unreachable();
			}
		}break;
		
		case nix::tPrimOp:{
			const Value& btin = state.eval().getBuiltins();
			CS(precedence::simple);
			for(auto& attr : *btin.attrs()){
				if(NixValueComparer::eq(&value, attr.value)){
					cs << "builtins." << STREAM(os, nix::printAttributeName(os, state.eval().symbols[attr.name]));
					return;
				}
			}
			state.eval().error<nix::EvalError>("could not resolve primitive operator %1%", value.primOp()->name).debugThrow();
		}break;
		
		case nix::tThunk:
		case nix::tUninitialized:
		case nix::tExternal:
		case nix::tFailed:
		case nix::tLambda:
		case nix::tNumberOfInternalTypes:
			state.eval().error<nix::EvalError>("cannot serialize value").debugThrow();
			break;
	}
}

template void formatValue(RewriteState& state, const BasicChartValueInfo* path, const FormatEnv* env, NewExpressionTarget target, const Value& value, bool skipCSE);
template void formatValue(RewriteState& state, const BasicChartValueInfo* path, const FormatEnv* env, IsolatedExpressionTarget target, const Value& value, bool skipCSE);
template void formatValue(RewriteState& state, const BasicChartValueInfo* path, const FormatEnv* env, ExistingExpressionTarget target, const Value& value, bool skipCSE);

template<typename TargetBindings>
void generateRewrites(RewriteState& state, TargetBindings& target, const nix::Bindings& bindings, bool writeExisting, bool writeNew){
	nix::ExprAttrs* a = target.attrs();
	std::vector<std::pair<nix::Symbol,const nix::Value*>> values;
	values.reserve(bindings.size());
	for(const auto& pair : bindings){
		if(a->attrs->contains(pair.name) ? writeExisting : writeNew){
			deeperForce(state.eval(), *pair.value);
			values.push_back({pair.name, state.cache.internPool.intern(pair.value)});
		}
	}
	if(target.isRecursive()){
		FormatEnv env;
		for(auto [name, value] : values){
			env.addSymbol(state, name, value);
		}
		for(auto [name, value] : values){
			BasicChartValueInfo pt;
			pt.pathName = name;
			pt.pathParent = path_env;
			std::visit([&](auto targetAttr){
				formatValue(state, &pt, &env, std::move(targetAttr), *value, false);
			}, target.makeStaticAttribute(state, name));
		}
	}else{
		for(auto [name, value] : values){
			std::visit([&](auto targetAttr){
				formatValue(state, path_unreachable, nullptr, std::move(targetAttr), *value, false);
			}, target.makeStaticAttribute(state, name));
		}
	}
}

template void generateRewrites(RewriteState&,ExistingBindingsTarget&,const nix::Bindings&,bool,bool);
template void generateRewrites(RewriteState&,IsolatedBindingsTarget&,const nix::Bindings&,bool,bool);
template void generateRewrites(RewriteState&,IsolatedLetTarget&,const nix::Bindings&,bool,bool);

}