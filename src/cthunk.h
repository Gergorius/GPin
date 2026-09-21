#pragma once

#include <nix/expr/nixexpr.hh>
#include <nix/expr/value.hh>

struct CustomThunk : nix::ExternalValueBase{
	virtual void eval(nix::EvalState& state, nix::Value& v) = 0;
	inline virtual std::ostream & print(std::ostream & str) const override{
		return str << "<custom thunk>";
	}
	inline virtual std::string showType() const override{
		return "<custom thunk type>";
	}
	inline virtual std::string typeOf() const override{
		return "customthunk";
	}
};

// Turn the CustomThunk into a value. It will be gc referenced.
void mkCustomThunk(nix::EvalState& state, CustomThunk* target, nix::Value& val);
