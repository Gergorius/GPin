#include "update.h"
#include "steal.h"
#include <fstream>
#include <iostream>
#include <nix/cmd/command.hh>
#include <nix/cmd/common-eval-args.hh>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-gc.hh>
#include <nix/expr/value.hh>
#include <nix/main/common-args.hh>
#include <nix/main/shared.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/args.hh>
#include <nix/util/exit.hh>
#include <nix/util/ref.hh>
#include <nix/util/source-path.hh>
#include <nix/util/types.hh>
#include <sstream>
#include <vector>

EXPORT_PRIVATE_MEMBER(getInnerAutoArgs, &nix::MixEvalArgs::autoArgs);

using nix::ref;
using nix::Strings;

struct GPinCommand : virtual nix::RootArgs, virtual nix::StoreCommand, virtual nix::MixEvalArgs{
    std::shared_ptr<nix::Store> evalStore;
    std::shared_ptr<EvalStateForUpdate> evalState;
	std::vector<std::string> targetFiles;
	bool helpRequested = false;

	GPinCommand();

	ref<nix::Store> getEvalStore(){
		if(!evalStore){
			evalStore = evalStoreUrl ? openStore(nix::StoreReference{*evalStoreUrl}) : getStore();
		}
		return ref<nix::Store>(evalStore);
	}
	ref<EvalStateForUpdate> getEvalState(){
		if(!evalState) {
			evalState = std::allocate_shared<EvalStateForUpdate>(traceable_allocator<EvalStateForUpdate>(), lookupPath, getEvalStore(), nix::fetchSettings, nix::evalSettings, getStore());
			evalState->repair = repair;
		}
   		return ref<EvalStateForUpdate>(evalState);
	}
	virtual void run(ref<nix::Store>) override;
};

GPinCommand::GPinCommand(){
	this->expectArgs("files",&this->targetFiles);
	this->removeFlag("arg");
	this->removeFlag("argstr");
	this->removeFlag("arg-from-file");
	this->removeFlag("arg-from-stdin");
}

void GPinCommand::run(ref<nix::Store>){
	if(!std::invoke(getInnerAutoArgs,*this).empty()){
		std::cerr << "Nice try, but autoargs are not accepted." << std::endl;
		throw nix::Exit(1);
	}

	EvalStateForUpdate* state = getEvalState().operator->();

	state->updateSymbol = state->symbols.create("update");

	nix::Bindings* autoArgs = getAutoArgs(*state);
	
	if(!autoArgs->empty()){
		nix::Value* autoArg = state->allocValue();
		autoArg->mkAttrs(autoArgs);
		state->autoArgument = nix::allocRootValue(autoArg);
	}

	std::vector<nix::SourcePath> sourcePaths;

	if(targetFiles.empty()){

		std::string input = std::string(std::istreambuf_iterator<char>(std::cin),std::istreambuf_iterator<char>{});
		auto& info = state->loadString(state->rootPath("."), std::move(input), true);

		state->finishLoad();

		state->doRewrite(std::cout, info);
		std::cout.flush();
	}else{

		for(std::string tf : targetFiles){
			state->loadFile(nix::lookupFileArg(*state, tf));
		}

		state->finishLoad();

		std::vector<std::pair<std::filesystem::path,std::string>> writeTasks;

		// NOT writing any files if there is an error is entirely reasonable.

		bool hasSoftError = false;

		for(auto& pairing : state->fileSources){
			const nix::SourcePath& sp = pairing.first;
			auto physicalPath = sp.getPhysicalPath();
			if(!physicalPath){
				std::cerr << "Error: could not resolve path for writing: " << sp.to_string() << std::endl;
				hasSoftError = true;
			}
			std::ostringstream ostr;
			state->doRewrite(ostr, pairing.second);
			writeTasks.push_back({std::move(physicalPath).value(), std::move(ostr).str()});
		}

		if(hasSoftError){
			throw nix::Exit(1);
		}

		for(auto& p : writeTasks){
			std::ofstream f{p.first};
			f << p.second;
			f.close();
		}
	}
}

void mainWrapped(int argc, char** argv){
	GPinCommand command{};

	command.parseCmdline(nix::argvToStrings(argc, argv));

	((nix::Command&)command).run();
}

int main(int argc, char ** argv){
	nix::initNix();
	nix::initGC();

	shared_ptr<nix::Store> store = nix::openStore();

	return nix::handleExceptions(argv[0], [&]() { mainWrapped(argc, argv); });
}