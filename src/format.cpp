
#include "format.h"
#include "altparse.h"
#include "treewalk.h"
#include "valueutil.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <nix/expr/eval-error.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/value.hh>
#include <nix/util/source-accessor.hh>
#include <nix/util/source-path.hh>
#include <string_view>
#include <utility>
#include <variant>

#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wimplicit-fallthrough"

using std::string;
using std::string_view;

#define GET_ADI [](const auto& v) -> const AttrsDeclarationInfo&{ return v; }

std::pair<string_view,string_view> detectIndentation(string_view source){
	string_view preindent = {};
	std::unordered_map<string_view, uint32_t> frequencyMap;
	std::optional<string_view> prevLine;
	using pos = string_view::size_type;

	while(source.size() != 0){
		pos c = source.find_first_of('\n');
		string_view line1;
		if(c == string_view::npos){
			line1 = source;
			source = {};
		}else{
			line1 = source.substr(0, c);
			source = source.substr(c + 1);
		}
		pos x = line1.find_first_not_of("\t ");
		if(x != string_view::npos){
			line1 = line1.substr(0,x);
		}

		if(prevLine){
			string_view line0 = prevLine.value();

			uint32_t i = 0;
			while(i < line0.size() && i < line1.size() && line0[i] == line1[i]){
				i++;
			}

			frequencyMap.try_emplace(line0.substr(i),0).first->second++;
			frequencyMap.try_emplace(line1.substr(i),0).first->second++;
			
			uint32_t j = 0;
			while(j < line1.size() && j < preindent.size() && line1[j] == preindent[j]){
				j++;
			}
			preindent = preindent.substr(0, j);
		}else{
			preindent = line1;
		}

		prevLine.emplace(line1);
	}
	
	std::optional<std::pair<string_view, uint32_t>> bestPair;

	for(auto& pair : frequencyMap){
		if(!pair.first.empty() && (!bestPair || bestPair->second < pair.second)){
			bestPair = pair;
		}
	}
	if(bestPair){
		return {preindent, bestPair->first};
	}else{
		return { {}, {} };
	}
}

std::pair<string_view,string_view> detectIndentation(RewriteState& state, uint32_t begin, uint32_t end){
	using pos = string_view::size_type;
	pos lineStart = state.source.substr(0, begin).find_last_of('\n');
	lineStart = lineStart == string_view::npos ? 0 : lineStart + 1;
	return detectIndentation(state.source.substr(lineStart, end - lineStart));
}

struct chart_info{
	std::vector<nix::Symbol> path;
	uint32_t strength; // Already existing paths take priority over paths we make.
	inline chart_info(): path(), strength(0){}
};

void chart(nix::Expr* expr, nix::Value* value, std::vector<nix::Symbol>& path, std::unordered_map<nix::Value*, chart_info>& paths){
	if(!path.empty()){
		auto& info = paths[value];
		int strength = expr == nullptr ? 1 : 2;
		if(info.strength < strength || info.strength == strength && path.size() < info.path.size()){
			info.path = path;
			info.strength = strength;
		}else{
			return;
		}
	}
	if(value->type() != nix::nAttrs){
		return;
	}
	const nix::ExprAttrs* attrs = dynamic_cast<nix::ExprAttrs*>(expr);
	for(auto& bnd : *value->attrs()){
		nix::Expr* subexpr = nullptr;
		if(attrs != nullptr){
			auto itr = attrs->attrs->find(bnd.name);
			if(itr != attrs->attrs->end()){
				subexpr = itr->second.e;
			}
		}
		path.push_back(bnd.name);
		chart(subexpr, bnd.value, path, paths);
		path.pop_back();
	}
}

bool FormatState::newLine(){
	if(indent.size() == 0){
		return false; // No new lines.
	}
	output << '\n' << preindent;
	for(int32_t i = 0;i < indentRepeatCount;i++){
		output << indent;
	}
	return true;
}

Rewrite FormatState::toRewrite() && {
	return Rewrite{
		.begin = begin,
		.end = end,
		.replacement = std::move(output).str()
	};
}

void formatValue(FormatState& state, nix::Value& value);

void formatBinding(FormatState& state, const nix::Attr& attr){
	state.newLine();
	nix::printIdentifier(state.output, state.eval.symbols[attr.name]);
	state.output << " = ";
	state.atomic = false;
	formatValue(state, *attr.value);
	state.output << ";";
}

void formatValue(FormatState& state, nix::Value& value){
begin:
	switch(internalType(value)){
		case nix::tInt:
		case nix::tBool:
		case nix::tString: // Indented strings are bugged and fix folk refuse to nix them so we will not use them.
		case nix::tNull:
			value.print(state.eval, state.output);
			break;
		
		case nix::tPath:{
			nix::SourcePath sp = value.path();
			if(sp.accessor == state.basePath.accessor){
				const std::string& base = state.basePath.path.abs();
				const std::string& subp = sp.path.abs();
				if(subp.starts_with(base)){
					state.output << "." << subp.substr(base.size());
				}else{
					state.output << sp.to_string();
				}
			}else{
				state.output << sp.to_string();
			}
		}break;
		
		case nix::tFloat:{
			// Why is this so complicated?
			auto f = value.fpoint();
			bool brackets = (f < 0.0 || std::isnan(f) || std::isinf(f)) && state.atomic;
			if(brackets){
				state.output << "(";
			}
			if(std::isnan(f)){
				state.output << "1.e308*2*0";
			}else if(std::isinf(f)){
				if(std::signbit(f)){
					state.output << "-1.e308*2";
				}else{
					state.output << "1.e308*2";
				}
			}else{
				std::print(state.output, "{:-#}", f);
			}
			if(brackets){
				state.output << ")";
			}
		}break;
		
		case nix::tAttrs:{
			const nix::Bindings& bindings = *value.attrs();
			if(bindings.size() == 0){
				state.output << "{}";
				break;
			}
			state.output << "{";
			state.indentRepeatCount++;
			for(auto& binding : bindings){
				formatBinding(state, binding);
			}
			state.indentRepeatCount--;
			state.newLine();
			state.output << "}";
		}break;
		
		case nix::tListN:
		case nix::tListSmall:{
			nix::ListView list = value.listView();
			if(list.size() == 0){
				state.output << "[]";
				break;
			}
			state.output << "[";
			state.indentRepeatCount++;
			bool notFirst = false;
			for(auto& val : list){
				if(!state.newLine() && notFirst){
					state.output << " ";
				}
				state.atomic = true;
				formatValue(state, *val);
				notFirst = true;
			}
			state.indentRepeatCount--;
			state.newLine();
			state.output << "]";
		}break;

		case nix::tPrimOp:{
			const nix::Value& btin = state.eval.getBuiltins();
			for(auto& attr : *btin.attrs()){
				if(NixValueComparer::eq(&value, attr.value)){
					state.output << "builtins.";
					nix::printIdentifier(state.output, state.eval.symbols[attr.name]);
					return;
				}
			}
			state.eval.error<nix::EvalError>("Could not resolve primitive operator %1%", value.primOp()->name).debugThrow();
		}break;
		
		case nix::tPrimOpApp:{
			bool brackets = state.atomic;
			if(brackets){
				state.output << "(";
			}
			state.atomic = false;
			formatValue(state, *value.primOpApp().left);
			state.output << " ";
			state.atomic = true;
			formatValue(state, *value.primOpApp().right);
			if(brackets){
				state.output << ")";
			}
		}break;
		
		case nix::tThunk:
		case nix::tUninitialized:
		case nix::tApp:
			state.eval.error<nix::EvalError>("unexpected value").panic();
			break;
		case nix::tExternal:
		case nix::tFailed:
		case nix::tLambda:
		case nix::tNumberOfInternalTypes:
			state.eval.error<nix::EvalError>("cannot serialize value").debugThrow();
			break;
			break;
	}
}

void eraseDeclaration(RewriteState& state, const AttrsDeclarationInfo& info){
	uint32_t endpos = info.endsemi->end;
	if(state.source[endpos] == '\n'){
		endpos++;
		while(state.source[endpos] == ' ' || state.source[endpos] == '\t'){
			endpos++;
		}
	}
	state.addRewrite(Rewrite{
		.begin = info.begin->begin,
		.end = endpos,
		.replacement = ""
	});
}

// Erase a non-isolated expression.
void eraseNonIsolated(RewriteState& state, SyntaxReference expr){
	std::vector<AttributeDeclaration> decvec = expr.findAllDeclarations(state.eval);

	for(uint32_t i = 0;i < decvec.size();i++){
		eraseDeclaration(state, std::visit(GET_ADI,decvec[i]));
	}
}

void eraseDynamicAndInheritAttrs(RewriteState& state, SyntaxReference& expr){
	std::unordered_set<uint32_t> positions;
	for(auto& exp : expr.attrs()->attrs.value()){
		if(exp.second.chooseByKind(false, true, true)){
			positions.insert(expr.origin.offsetOf(exp.second.pos));
		}
	}
	for(auto& dyn : expr.attrs()->dynamicAttrs.value()){
		positions.insert(expr.origin.offsetOf(dyn.pos));
	}
	if(positions.empty()){
		return;
	}
	auto result = expr.findMemberDeclarationsContaining(state.eval, positions);
	for(const auto& r : result){
		eraseDeclaration(state, std::visit(GET_ADI,r));
	}
}

// Find or create the body of the attribute set represented by the specified expression.
template<bool mustExist = false>
std::conditional_t<mustExist,void,uint32_t> findOrDeclareAttributeSet(RewriteState& rs, FormatState& state, const SyntaxReference& expr){
	std::span<const NixToken> boundary;
	const NixToken* cursor;
	if(expr.isIsolated()){
		boundary = expr.boundary;
		cursor = expr.boundary.data();
		goto walkToBegin;
	}
	{
		std::vector<AttributeDeclaration> decvec = expr.findAllDeclarations(state.eval);
		for(uint32_t i = 0;i < decvec.size();i++){
			BindingInfo& info = std::get<BindingInfo>(decvec[i]);
			if(info.doteq->type == '='){
				boundary = { info.begin, info.endsemi };
				cursor = info.doteq;
				goto walkToBegin;
			}
		}
		if constexpr(mustExist){
			rs.eval.error<nix::EvalError>("expected to find an attribute set").panic();
			return;
		}else{
			const SyntaxReference parent = expr.getParent();
			uint32_t depth = findOrDeclareAttributeSet(rs, state, parent);
			state.newLine();
			nix::printIdentifier(state.output, state.eval.symbols[expr.path->name.symbol]);
			state.output << " = {";
			return depth + 1;
		}
	}
walkToBegin:
	std::tie(state.preindent,state.indent) = detectIndentation(rs, boundary[0].begin, boundary[boundary.size() - 1].end);
	while(cursor->type != '{'){
		cursor++;
	}
	state.atomic = true;
	state.indentRepeatCount = 1;
	state.begin = cursor->end;
	state.end = cursor->end;
	if constexpr(!mustExist) return 0;
}

// Prepare for completely rewriting the non-isolated expression. If it does not exist, declare an attribute for it. If it exists, isolate it by emitting rewrites. The caller must remember to add the ending semicolon.
void findOrDeclareAttribute(RewriteState& rs, FormatState& state, SyntaxReference expr){

	std::vector<AttributeDeclaration> decvec = expr.findAllDeclarations(state.eval);

	uint32_t i = 0;

	if(decvec.empty() || expr.isInherit()){ // Inherit just won't do for us.
		const SyntaxReference parent = expr.getParent();

		findOrDeclareAttributeSet<true>(rs, state, parent);

		state.newLine();
		nix::printIdentifier(state.output, state.eval.symbols[expr.path->name.symbol]);
		state.output << " = ";
	}else{
		const BindingInfo& info = std::get<BindingInfo>(decvec[i++]);

		if(info.doteq->type != '='){
			BindingInfo si = info.next();
			while(si.doteq->type == '.'){
				si = si.next();
			}
			rs.addRewrite(Rewrite{
				.begin = info.doteq->begin,
				.end = (si.doteq - 1)->end,
				.replacement = ""
			});
			state.begin = si.doteq[1].begin;
		}else{
			state.begin = info.doteq[1].begin;
		}
		state.end = info.endsemi->end;

		std::tie(state.preindent, state.indent) = detectIndentation(rs, state.begin, state.end);

	}

	while(i < decvec.size()){
		auto d = decvec[i++];

		if(std::holds_alternative<BindingInfo>(d)){
			eraseDeclaration(rs, std::get<BindingInfo>(d));
		}else{
			// Remove the expression from the inherit.
			const NixToken* begin = expr.binarySearchPos(expr.expression->getPos());
			const NixToken* end = begin;
			maybeWalkToClosingToken(end);
			rs.addRewrite({
				.begin = (begin - 1)->end, // ...politely.
				.end = end->end,
				.replacement = ""
			});
		}
	}
}

void finishAttributeSetDeclaration(FormatState& state, uint32_t depth){
	for(uint32_t d = 0;d < depth;d++){
		state.indentRepeatCount--;
		state.newLine();
		state.output << "}";
	}
}

void generateRewrite(RewriteState& state, SyntaxReference expr, nix::Value& value){
	// Assume value is deep forced.

	if(value.type() == nix::nAttrs){
		const nix::ExprAttrs* attrs = dynamic_cast<nix::ExprAttrs*>(expr.expression);
		if(!attrs){
			goto defaultRewrite;
		}
		const nix::Bindings& bindings = *value.attrs();
		if(bindings.empty()){
			goto defaultRewrite;
		}
		eraseDynamicAndInheritAttrs(state, expr);
		for(auto& attrDef : attrs->attrs.value()){
			if(bindings.get(attrDef.first) == nullptr && attrDef.second.chooseByKind(true, false, false)){
				SubexpressionFrame frame;
				eraseNonIsolated(state, expr.getSubexpression(&frame, attrDef.first));
			}
		}

		bool hasNewBindings = false;
		for(auto& binding : bindings){
			if(attrs->attrs->contains(binding.name)){
				SubexpressionFrame frame;
				AttrPathNode pn;
				generateRewrite(state, expr.getSubexpression(&frame, binding.name), *binding.value);
			}else{
				hasNewBindings = true;
			}
		}

		if(hasNewBindings){
			FormatState f{
				.eval = state.eval,
				.basePath = state.basePath
			};

			uint32_t depth = findOrDeclareAttributeSet(state, f, expr);

			for(auto& binding : bindings){
				if(!attrs->attrs->contains(binding.name)){
					formatBinding(f, binding);
				}
			}

			finishAttributeSetDeclaration(f, depth);

			state.addRewrite(std::move(f).toRewrite());
		}
		return;
	}

defaultRewrite:
	using pos = string_view::size_type;

	FormatState f{
		.eval = state.eval,
		.basePath = state.basePath
	};

	bool endSemi = false;

	if(expr.tryIsolate()){
		f.begin = expr.beginOffset();
		f.end = expr.endOffset();
		std::tie(f.preindent, f.indent) = detectIndentation(state, f.begin, f.end);
	}else{
		findOrDeclareAttribute(state, f, std::move(expr));
		endSemi = true;
	}

	std::ostringstream stream;

	formatValue(f, value);

	if(endSemi){
		f.output << ";";
	}

	state.addRewrite(std::move(f).toRewrite());
}

/*
struct rec_info{
	uint32_t depthEntered;
	bool recursive;
	bool exited;
	rec_info() : depthEntered(0xFFFFFFFF), recursive(false), exited(false){}
};

struct nix_value_hash{
	inline size_t operator()(const nix::Value* v) const{
		size_t acc = v->type();
		switch(v->type()){
			case nix::nThunk:
			case nix::nFailed:
				return acc;
			case nix::nInt:
				return acc ^ std::hash<nix::NixInt::Inner>{}(v->integer().value);
			case nix::nFloat:
				return acc ^ std::hash<nix::NixFloat>{}(v->fpoint());
			case nix::nBool:
				return acc ^ std::hash<bool>{}(v->boolean());
			case nix::nString:
				return acc ^ std::hash<string_view>{}(v->string_view());
			case nix::nPath:
				return acc ^ std::hash<string_view>{}(v->pathStrView());
			case nix::nNull:
				return acc;
			case nix::nAttrs:
				for(auto& binding : *v->attrs()){
					acc = acc ^ (binding.name.getId() * 31 + binding.value->type());
				}
				return acc;
			case nix::nList:
				for(auto& value : v->listView()){
					acc = acc * 31 + value->type();
				}
				return acc;
			case nix::nFunction:
				return acc;
			case nix::nExternal:
				return acc;
		}
     }
};

inline uint32_t detectRecursiveValues(std::unordered_map<nix::Value*, rec_info>& seen,uint32_t depth,nix::Value* value){
	uint32_t minDepth = 0xFFFFFFFF;
	auto vtype = value->type();
	if(vtype == nix::nAttrs || vtype == nix::nList){
		rec_info& info = seen[value];

		if(info.exited){
			return 0xFFFFFFFF;
		}

		if(info.depthEntered != 0xFFFFFFFF){
			return info.depthEntered;
		}
		info.depthEntered = depth;

		if(vtype == nix::nAttrs){
			for(auto& b : *value->attrs()){
				minDepth = std::min(minDepth,detectRecursiveValues(seen, depth + 1, b.value));
			}
		}else{
			for(auto& v : value->listView()){
				minDepth = std::min(minDepth,detectRecursiveValues(seen, depth + 1, v));
			}
		}

		info.depthEntered = 0xFFFFFFFF;
		if(minDepth <= depth){
			info.recursive = true;
		}
		info.exited = true;
	}
	return minDepth;
}
*/