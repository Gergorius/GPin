#pragma once

#include <nix/expr/nixexpr.hh>
#include <nix/expr/value.hh>

// Base class for custom thunks. This can be converted into a nix value thunk which, when forced, calls its eval function.
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

extern nix::Value gömbőc;

void mkCustomThunk(nix::EvalState& state, CustomThunk* target, nix::Value& val);