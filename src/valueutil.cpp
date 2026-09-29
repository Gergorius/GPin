
#include "valueutil.h"
#include <nix/expr/attr-set.hh>
#include <nix/expr/value.hh>
#include <nix/util/source-path.hh>
#include <deque>
#include <unordered_set>
#include <boost/container_hash/hash.hpp>

size_t NixValueHash::operator()(nix::Value* vptr) const{
	size_t sum = std::hash<nix::ValueType>{}(vptr->type()) * 31;
	switch(vptr->type()){
		case nix::nInt:
			sum += std::hash<nix::NixInt::Inner>{}(vptr->integer().value);
			break;
		case nix::nFloat:
			sum += std::hash<nix::NixFloat>{}(vptr->fpoint());
			break;
		case nix::nBool:
			sum += std::hash<bool>{}(vptr->boolean());
			break;
		case nix::nString:
			sum += std::hash<std::string_view>{}(vptr->string_view());
			break;
		case nix::nPath:
			sum += std::hash<std::string_view>{}(vptr->pathStrView());
			break;
		case nix::nAttrs:
			for(auto& attr : *vptr->attrs()){
				sum = sum ^ (std::hash<uint32_t>{}(attr.name.getId()) * 31 + std::hash<nix::ValueType>{}(attr.value->type()));
			}
			break;
		case nix::nList:
			for(nix::Value* elem : vptr->listView()){
				sum = (sum + std::hash<nix::ValueType>{}(elem->type())) * 31;
			}
			break;
		case nix::nFunction:{
			if(vptr->isPrimOp()){
				sum = sum + std::hash<const void*>{}(vptr->primOp());
			}else if(vptr->isLambda()){
				sum = (sum + std::hash<void*>{}(vptr->lambda().env)) * 31 + std::hash<void*>{}(vptr->lambda().fun);
			}else if(vptr->isPrimOpApp()){
				sum = sum + (*this)(vptr->primOpApp().left) * 31 + (*this)(vptr->primOpApp().right);
			}
			}break;
		case nix::nExternal:
			sum = sum + std::hash<std::string>{}(vptr->external()->showType());
			break;
		case nix::nThunk:
			if(vptr->isThunk()){
				sum = (sum + std::hash<const void*>{}(vptr->thunk().env)) * 31 + std::hash<const void*>{}(vptr->thunk().expr);
			}
		case nix::nFailed:
		case nix::nNull:
			break;
	}
	return sum;
}

bool NixValueEq::operator()(nix::Value* l,nix::Value* r) const{
#define GUARD(a,b) if(a != b){ return false; }
	if(l == r){
		return true;
	}
	GUARD(l->type(),r->type());

	using pair_type = std::pair<nix::Value*,nix::Value*>;

	std::array<std::byte, 0x5000> buf;

	std::pmr::monotonic_buffer_resource res{buf.data(),buf.size()};

	std::pmr::deque<pair_type> queue{&res};
	std::pmr::unordered_set<pair_type,boost::hash<pair_type>> seen{&res};

	seen.insert({l,r});

#define RECURSIVE_EQ(a,b) if((a) != (b)){ GUARD((a)->type(),(b)->type()); if(!seen.insert({(a),(b)}).second){ queue.push_back({(a),(b)}); }}

	while(true){
		switch(l->type()){
			case nix::nInt:
				GUARD(l->integer(),r->integer());
				break;
			case nix::nFloat:{
				if(std::isnan(l->fpoint()) && std::isnan(r->fpoint())){
					break; // Reflexivity is more important to us than "accuracy".
				}
				// That being said, we do not tell negative and positive zeroes apart because neither does nix.
				GUARD(l->fpoint(),r->fpoint());
				}break;
			case nix::nBool:
				GUARD(l->boolean(),r->boolean());
				break;
			case nix::nString:
				GUARD(l->string_view(),r->string_view());
				break;
			case nix::nPath:
				GUARD(*l->path().accessor,*r->path().accessor);
				GUARD(l->path().path,r->path().path);
				break;
			case nix::nNull:
				break;
			case nix::nThunk:
				if(l->isApp() && r->isApp()){
					RECURSIVE_EQ(l->app().left, r->app().right);
					RECURSIVE_EQ(l->app().right, r->app().right);
					break;
				}else if(l->isThunk() && r->isThunk()){
					GUARD(l->thunk().env,r->thunk().env);
					[[unlikely]];
					GUARD(l->thunk().expr,r->thunk().expr);
					break;
				}
				return false;
			case nix::nAttrs:{
				const nix::Bindings& lb = *l->attrs();
				const nix::Bindings& rb = *r->attrs();
				GUARD(lb.size(),rb.size());
				auto li = lb.begin();
				auto ri = rb.begin();
				for(size_t i = 0;i < lb.size();i++){
					GUARD(li->name,ri->name);
					RECURSIVE_EQ(li->value, ri->value);
					li++;
					ri++;
				}
				}break;
			case nix::nList:{
				auto li = l->listView();
				auto ri = r->listView();
				GUARD(li.size(),ri.size());
				for(size_t i = 0;i < li.size();i++){
					RECURSIVE_EQ(li[i], ri[i]);
				}
				}break;
			case nix::nFunction:
				if(l->isLambda() && r->isLambda()){
					GUARD(l->lambda().env,r->lambda().env);
					GUARD(l->lambda().fun,r->lambda().fun);
					break;
				}else if(l->isPrimOp() && r->isPrimOp()){
					GUARD(l->primOp(),r->primOp());
					break;
				}else if(l->isPrimOpApp() && r->isPrimOpApp()){
					RECURSIVE_EQ(l->primOpApp().left, r->primOpApp().right);
					RECURSIVE_EQ(l->primOpApp().right, r->primOpApp().right);
					break;
				}
				return false;
			case nix::nExternal:
				GUARD(*l->external(),*r->external());
				break;
			case nix::nFailed:
				return false;
		}

		if(queue.empty()){
			return true;
		}
		std::tie(l,r) = queue.back();
		queue.pop_back();
	}
}