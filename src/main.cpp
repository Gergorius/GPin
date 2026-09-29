#include "update.h"
#include "steal.h"
#include <fstream>
#include <iostream>
#include <nix/cmd/common-eval-args.hh>
#include <nix/expr/eval-gc.hh>
#include <nix/main/shared.hh>
#include <nix/store/store-open.hh>
#include <nix/util/args.hh>
#include <nix/util/exit.hh>
#include <nix/util/source-path.hh>
#include <nix/util/types.hh>
#include <sstream>
#include <vector>

EXPORT_PRIVATE_MEMBER(getAutoArgs, &nix::MixEvalArgs::autoArgs);

using nix::Strings;

void mainWrapped(int argc, char** argv){
	nix::initNix();
	nix::initGC();

	Strings targetFiles;

	struct MyArgs : nix::LegacyArgs, nix::MixEvalArgs
	{
		using LegacyArgs::LegacyArgs;
	};

	MyArgs myArgs(std::string(nix::baseNameOf(argv[0])), [&](Strings::iterator & arg, const Strings::iterator & end){
		if(*arg != "" && arg->at(0) == '-'){
			return false;
		}else{
			targetFiles.push_back(*arg);
		}
		return true;
	});

	myArgs.removeFlag("arg");
	myArgs.removeFlag("argstr");
	myArgs.removeFlag("arg-from-file");
	myArgs.removeFlag("arg-from-stdin");

	myArgs.parseCmdline(nix::argvToStrings(argc, argv));

	if(!std::invoke(getAutoArgs,myArgs).empty()){
		std::cerr << "Auto args are not supported by gpin-update!" << std::endl;
		throw nix::Exit(1);
	}
	
	auto store = nix::openStore();

	std::shared_ptr<EvalStateForUpdate> state = std::make_shared<EvalStateForUpdate>(myArgs.lookupPath, store, nix::fetchSettings, nix::evalSettings, store);

	state->repair = myArgs.repair;

	state->updateSymbol = state->symbols.create("update");

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

int main(int argc, char ** argv){
	nix::initNix();
	nix::initGC();

	shared_ptr<nix::Store> store = nix::openStore();

	return nix::handleExceptions(argv[0], [&]() { mainWrapped(argc, argv); });
}