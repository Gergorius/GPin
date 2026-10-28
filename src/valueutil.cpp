
#include "valueutil.h"
#include "steal.h"

#include <cmath>
#include <gc/gc_allocator.h>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/value.hh>
#include <nix/util/error.hh>
#include <nix/util/fmt.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/source-path.hh>
#include <stack>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <boost/container_hash/hash.hpp>

namespace gpin{

// If these change I better know...
static_assert(std::is_trivially_destructible_v<nix::Value>);
static_assert(std::is_trivially_copy_constructible_v<nix::Value>);
static_assert(std::is_trivially_copy_assignable_v<nix::Value>);
static_assert(std::is_trivially_move_constructible_v<nix::Value>);
static_assert(std::is_trivially_move_assignable_v<nix::Value>);
static_assert(std::is_same_v<nix::NixInt::Inner, int64_t>);

EXPORT_PRIVATE_MEMBER(getInternalType,&nix::Value::getInternalType);

// The formatter wants to know about context, therefore we do not consider strings with different context equal.
#define EQ_CONSIDERS_STRING_CONTEXT

using nix::InternalType;

InternalType internalType(const nix::Value& v){
	return std::invoke(getInternalType,v);
}

#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wimplicit-fallthrough"

static void unusedButMustCompile(nix::Value& value){

	// Due to the warnings we just set if the enumeration changes this will produce a compile time error.

	switch(internalType(value)){
		case nix::tUninitialized:
		case nix::tInt:
		case nix::tBool:
		case nix::tNull:
		case nix::tFloat:
		case nix::tFailed:
		case nix::tExternal:
		case nix::tPrimOp:
		case nix::tAttrs:
		case nix::tListSmall:
		case nix::tPrimOpApp:
		case nix::tApp:
		case nix::tThunk:
		case nix::tLambda:
		case nix::tListN:
		case nix::tString:
		case nix::tPath:
		case nix::tNumberOfInternalTypes:
	}
}

size_t NixValueComparer::hash(const nix::Value* vptr){
	size_t sum = std::hash<nix::ValueType>{}(vptr->type()) * 31;
	switch(internalType(*vptr)){
		case nix::tInt:
			sum += std::hash<nix::NixInt::Inner>{}(vptr->integer().value);
			break;
		case nix::tBool:
			sum += std::hash<bool>{}(vptr->boolean());
			break;
		case nix::tFloat:
			sum += std::hash<nix::NixFloat>{}(vptr->fpoint());
			break;
		case nix::tExternal:{
			nix::ExternalValueBase* ext = vptr->external();
			if(auto* hev = dynamic_cast<HashableExternalValue*>(ext)){
				sum = sum + hev->getHashCode();
			}else{
				sum = sum + std::hash<const void*>{}(ext);
			}
			}break;
		case nix::tPrimOp:
			sum = sum + std::hash<const void*>{}(vptr->primOp());
			break;
		case nix::tAttrs:
			for(auto& attr : *vptr->attrs()){
				sum = sum ^ (std::hash<uint32_t>{}(attr.name.getId()) * 31 + std::hash<nix::ValueType>{}(attr.value->type()));
			}
			break;
		case nix::tPrimOpApp:
			sum = sum + hash(vptr->primOpApp().left) * 31 + hash(vptr->primOpApp().right);
			break;
		case nix::tThunk:
			sum = (sum + std::hash<const void*>{}(vptr->thunk().env)) * 31 + std::hash<const void*>{}(vptr->thunk().expr);
			break;
		case nix::tLambda:
			sum = (sum + std::hash<void*>{}(vptr->lambda().env)) * 31 + std::hash<void*>{}(vptr->lambda().fun);
			break;
		case nix::tString:
			sum += std::hash<std::string_view>{}(vptr->string_view());
			break;
		case nix::tPath:
			sum += std::hash<std::string_view>{}(vptr->pathStrView());
			break;
		case nix::tListN:
		case nix::tListSmall:
			for(nix::Value* elem : vptr->listView()){
				sum = (sum + std::hash<nix::ValueType>{}(elem->type())) * 31;
			}
			break;
		default:
			break;
	}
	return sum;
}

void forceForHashing(nix::EvalState& state, nix::Value& value){
begin:
	switch(internalType(value)){
		case nix::tAttrs:
			for(auto& binding : *value.attrs()){
				state.forceValue(*binding.value, binding.pos);
			}
			break;
		case nix::tListN:
		case nix::tListSmall:
			for(auto& val : value.listView()){
				state.forceValue(*val, nix::noPos);
			}
			break;
		case nix::tPrimOpApp:
			forceForHashing(state, *value.primOpApp().left);
			forceForHashing(state, *value.primOpApp().right);
			break;
		case nix::tApp:
		case nix::tThunk:
			state.forceValue(value, nix::noPos);
			goto begin;
		default:
			break;
	}
}

static bool stringContextEq(const nix::Value* l,const nix::Value* r){
	if(l->context() == r->context()){
		return true;
	}else if(l->context() == nullptr || r->context() == nullptr){
		return false;
	}else if(l->context()->size() != r->context()->size()){
		return false;
	}else{
		nix::NixStringContext ca;
		nix::NixStringContext cb;
		nix::copyContext(*l, ca);
		nix::copyContext(*r, cb);
		return ca == cb;
	}
}

struct empty{};

template<bool doForce,typename VPtr>
static bool eqValues(VPtr l,VPtr r,std::conditional_t<doForce, nix::EvalState&, empty> state){
	using pair_type = std::pair<VPtr,VPtr>;

	std::array<std::byte, 0x1000> buf;
	std::pmr::monotonic_buffer_resource res{buf.data(),buf.size()};
	std::pmr::unordered_set<pair_type,boost::hash<pair_type>> seen{&res};

	return [&](this const auto & recurse,VPtr l,VPtr r){
		if(l == r){
			return true;
		}

#define GUARD(a,b) if(a != b){ return false; }
#define CHK(str) (l str == r str)
#define REC(str) (recurse((l str),(r str)))
	begin:
		if(internalType(*l) != internalType(*r)){
			goto recovery;
		}
		if(!seen.insert({l,r}).second){
			return true;
		}
		switch(internalType(*l)){
			case nix::tInt: return CHK(->integer());
			case nix::tFloat:
				if(std::isnan(l->fpoint()) && std::isnan(r->fpoint())){
					return true; // Reflexivity is more important to us than "accuracy".
				}
				// That being said, we do not tell negative and positive zeroes apart because neither does nix.
				return CHK(->fpoint());
			case nix::tBool: return CHK(->boolean());
			case nix::tString:
				GUARD(l->string_view(),r->string_view());
#ifdef EQ_CONSIDERS_STRING_CONTEXT
				if(!stringContextEq(l, r)) return false;
#endif
				return true;
			case nix::tPath: return CHK(->path());
			case nix::tNull: return true;
			case nix::tThunk:
				if(CHK(->thunk().env) && CHK(->thunk().expr)) return true;
				goto recovery;
			case nix::tApp: 
				if(REC(->app().left) && REC(->app().right)) return true;
				goto recovery;
			case nix::tAttrs:{
				GUARD(l->attrs()->size(),r->attrs()->size());
				for(size_t i = 0;i < l->attrs()->size();i++){
					GUARD((*l->attrs())[i].name,(*r->attrs())[i].name);
					if(!recurse((*l->attrs())[i].value,(*r->attrs())[i].value)){
						return false;
					}
				}
				return true;
			}
			case nix::tListSmall:
			case nix::tListN:{
				GUARD(l->listSize(),r->listSize());
				for(size_t i = 0;i < l->listSize();i++){
					if(!recurse(l->listView()[i],r->listView()[i])){
						return false;
					}
				}
				return true;
			}
			case nix::tLambda: return CHK(->lambda().env) && CHK(->lambda().fun);
			case nix::tPrimOp: return CHK(->primOp());
			case nix::tPrimOpApp: return REC(->primOpApp().left) && REC(->primOpApp().right);
			case nix::tExternal:{
				nix::ExternalValueBase* le = l->external();
				nix::ExternalValueBase* re = r->external();
				if(dynamic_cast<HashableExternalValue*>(le) && dynamic_cast<HashableExternalValue*>(re)){
					return (*le) == (*re);
				}else{
					return le == re;
				}
			}
			case nix::tFailed:
			case nix::tUninitialized:
			case nix::tNumberOfInternalTypes:
				goto recovery;
		}
	recovery:
		if constexpr(doForce){
			bool eitherForced = false;
			if(l->isThunk()){
				state.forceValue(*l, nix::noPos);
				eitherForced = true;
			}
			if(r->isThunk()){
				state.forceValue(*r, nix::noPos);
				eitherForced = true;
			}
			if(eitherForced){
				goto begin;
			}
		}
		return false;
	}(l,r);
}

#undef REC

bool NixValueComparer::eq(const nix::Value* l,const nix::Value* r){ return eqValues<false,const nix::Value*>(l,r,{}); }
bool LazyNixValueComparer::eq(nix::Value* l,nix::Value* r) const { return eqValues<true,nix::Value*>(l,r,state); }

void deeperForce(nix::EvalState& state, nix::Value& value){
	nix::Value* v = &value;

	std::unordered_set<nix::Value*> seen;

	[&state,&seen](this const auto & recurse, nix::Value* v){
		auto _cd = state.addCallDepth(v->determinePos(nix::noPos));

		if(!seen.insert(v).second)
			return;

		state.forceValue(*v, v->determinePos(nix::noPos));

		switch(internalType(*v)){
			case nix::tAttrs:
				for(auto& binding : *v->attrs()){
					try{
						recurse(binding.value);
					}catch(nix::Error& e){
						e.addTrace(state.positions[binding.pos], nix::HintFmt("while evaluating the attribute '%1%'", state.symbols[binding.name]));
						throw;
					}
				}
				break;
			case nix::tListSmall:
			case nix::tListN:
				for(uint32_t i = 0;i < v->listSize();i++){
					try{
						recurse(v->listView()[i]);
					}catch(nix::Error& e){
						e.addTrace(nullptr, nix::HintFmt("while evaluating list element at index %1%", i));
						throw;
					}
				}
				break;
			case nix::tPrimOpApp:
				do{
					try{
						recurse(v->primOpApp().right);
					}catch(nix::Error& e){
						uint32_t opIndex = 1;
						nix::Value* t = v->primOpApp().left;
						while(t->isPrimOpApp()){
							opIndex++;
							t = t->primOpApp().left;
						}
						e.addTrace(nullptr, nix::HintFmt("while evaluating operand %1% of %2%", opIndex, t->primOp()->name));
						throw;
					}
					v = v->primOpApp().left;
				}while(!v->isPrimOp());
				break;
			case nix::tApp:
			case nix::tThunk:
				nix::panic("EvalState::forceValue resulted in a thunk");
				break;
			default:
				break;
		}
	}(&value);
}

// Prevents intern from mutating input values at the cost of more memory usage.
#define INTERN_NO_SIDE_EFFECTS

std::pair<nix::Value*,bool> NixValueInternPool::recursiveIntern(nix::Value* value){

	// return.first: Pointer to canonical value.
#ifdef INTERN_NO_SIDE_EFFECTS
	// return.second: Whether the canonical value is by-pointer equal to the parameter value.
#else
	// return.second: Whether the canonical value is a PERFECT copy of the argument value. (Perfect means nix code cannot observe the difference.)
#endif

	const PoolPtr* p;
	bool perfect;
	{
		auto [itr, inserted] = pool.emplace(value);
		perfect = inserted;
		p = &*itr;
	}

#define PREP_REPLACEMENT if(!perfect){ perfect = p->value == value; break; } p->value = state.allocValue(); (*p->value) = *value

// Break-on-perfect-replacement.
#ifdef INTERN_NO_SIDE_EFFECTS
#define CHECK if(perfect){ p->value = value; break; }
#else
#define CHECK if(perfect){ p->value = value; }
#endif

#define REC(n,v) nix::Value* n; if(perfect){ std::tie(n,perfect) = recursiveIntern(v); }else{ n = recursiveIntern(v).first; }

	switch(internalType(*value)){

		// First we handle all values that have subvalues.

		case nix::tPrimOpApp:{

			PREP_REPLACEMENT;

			REC(l,value->primOpApp().left);
			REC(r,value->primOpApp().right);

			CHECK;

			p->value->mkPrimOpApp(l, r);
		}break;

		case nix::tApp:{

			PREP_REPLACEMENT;

			REC(l,value->app().left);
			REC(r,value->app().right);
			
			CHECK;

			p->value->mkApp(l, r);
		}break;

		case nix::tListSmall:
		case nix::tListN:{

			PREP_REPLACEMENT;
			
			nix::ListBuilder builder = state.buildList(value->listSize());
			for(size_t i = 0;i < value->listSize();i++){
				REC(v,value->listView()[i]);
				builder.elems[i] = v;
			}

			CHECK;

			p->value->mkList(builder);
		}break;

		case nix::tAttrs:{

			PREP_REPLACEMENT;

			nix::BindingsBuilder newBindings = state.buildBindings(value->attrs()->size());

			for(size_t i = 0;i < value->attrs()->size();i++){
				nix::Attr a = (*value->attrs())[i];
				REC(v, a.value);
				a.value = v;
				newBindings.push_back(a);
			}
			
			CHECK;

			p->value->mkAttrs(newBindings.alreadySorted());
		}break;

		// Deduplicating values that do not compare equal to themselves (according to nix! Not according to us.) produces an observable side effect.
		
		case nix::tLambda:
		case nix::tPrimOp:
		case nix::tThunk:
		case nix::tFailed:
			perfect = p->value == value;
			break;
		case nix::tFloat:
			perfect = p->value == value || !std::isnan(value->fpoint());
			break;
		
		// Deduplicating strings with different contexts produces an observable side effect. This is either solved by not considering them equal or marking them not perfectly equal.

		case nix::tString:
#ifndef EQ_CONSIDERS_STRING_CONTEXT
#ifndef INTERN_NO_SIDE_EFFECTS
			perfect = stringContextEq(p->value,value);
			break;
#endif
#endif
		
		// The following values can be deduplicated just fine.

		case nix::tInt:
		case nix::tBool:
		case nix::tNull:
		case nix::tPath:
		case nix::tExternal:
			perfect = true;
			break;
		
		// The following values are errors.

		case nix::tUninitialized:
		case nix::tNumberOfInternalTypes:
			nix::panic("unexpected value kind");
			break;
	}
#ifdef INTERN_NO_SIDE_EFFECTS
	return { p->value, p->value == value };
#else
	return { p->value, perfect };
#endif
}

const nix::Value* NixValueInternPool::intern(nix::Value* value){
	return recursiveIntern(value).first;
}

uint32_t NixValueTopology::lookupTopologyIndex(const nix::Value* ptr){
	{
		auto itr = componentIdMap.find(ptr);
		if(itr != componentIdMap.end()){
			return itr->second;
		}
		if(!isCompoundValue(ptr)){
			return 0;
		}
	}

	std::stack<const nix::Value*> stack;

	struct ComponentValueInfo{
		uint32_t id;
		uint32_t low;
		bool inStack;
	};

	std::unordered_map<const nix::Value*,ComponentValueInfo> data;

	constexpr uint32_t highIndex = 0xFFFFFFFFu;

	[&](this const auto& recurse, const nix::Value* value) -> uint32_t {
		if(!isCompoundValue(value)){
			return highIndex;
		}
		auto [itr, inserted] = data.insert({value, ComponentValueInfo{.id = (uint32_t)data.size(), .low = (uint32_t)data.size(), .inStack = true}});
		ComponentValueInfo& info = itr->second;
		if(!inserted){
			return info.inStack ? info.low : highIndex;
		}
		if(componentIdMap.contains(value)){
			info.inStack = false;
			return highIndex;
		}
		stack.push(value);
		bool referencesSelf = false;
		visitSubvalues(value, [&](const nix::Value* value2){
			referencesSelf |= value2 == value;
			info.low = std::min(info.low,recurse(value2));
			return false;
		});
		if(info.low == info.id){
			if(stack.top() == value && !referencesSelf){
				stack.pop();
				componentIdMap.insert({value,this->idCounter * 2});
			}else{
				const nix::Value* value2;
				do{
					value2 = stack.top();
					stack.pop();
					ComponentValueInfo& info2 = data.at(value2);
					info2.inStack = false;
					componentIdMap.insert({value2,this->idCounter * 2 + 1});
				}while(value2 != value);
			}
			this->idCounter++;
		}
		return info.low;
	}(ptr);

	return componentIdMap.at(ptr);
}

}