/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <fstream>
#include <iostream>
#include <map>
#include <utility>

#include "backends/taixuan/version.h"
#include "control-plane/p4RuntimeSerializer.h"
#include "ir/ir.h"
#include "ir/json_loader.h"
#include "lib/log.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/gc.h"
#include "lib/crash.h"
#include "lib/nullstream.h"
#include "frontends/common/applyOptionsPragmas.h"
#include "frontends/common/parseInput.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/frontend.h"
#include "frontends/p4/toP4/toP4.h"
#include "rewrite.h"
#include "pragmaModify.h"

#include "midend.h"

#include "z3++.h"
using namespace z3;

/**
   Demonstration of how Z3 can be used to prove validity of
   De Morgan's Duality Law: {e not(x and y) <-> (not x) or ( not y) }
*/
void demorgan() {
    std::cout << "de-Morgan example\n";
    
    context c;

    expr x = c.bool_const("x");
    expr y = c.bool_const("y");
    expr conjecture = (!(x && y)) == (!x || !y);
    
    solver s(c);
    // adding the negation of the conjecture as a constraint.
    s.add(!conjecture);
    std::cout << s << "\n";
    std::cout << s.to_smt2() << "\n";
    switch (s.check()) {
    case unsat:   std::cout << "de-Morgan is valid\n"; break;
    case sat:     std::cout << "de-Morgan is not valid\n"; break;
    case unknown: std::cout << "unknown\n"; break;
    }
}

class TaixuanOptions : public CompilerOptions {
 public:
    bool parseOnly = false;
    bool validateOnly = false;
    bool loadIRFromJson = false;
    bool prettyPrint = false;
    cstring input_folder_path = nullptr;
    cstring output_folder_path = nullptr;
    cstring pp_file = nullptr;
    std::vector<cstring> removedFilters;
    std::vector<Taixuan::PragmaModify::PragmaOption> insertedAnnotations;
    cstring pragmaOutputPath = nullptr;
    bool pragmaModify = false;
    cstring phv_file = nullptr;
    TaixuanOptions() {
        registerOption("--listMidendPasses", nullptr,
                [this](const char*) {
                    listMidendPasses = true;
                    loadIRFromJson = false;
                    Taixuan::MidEnd MidEnd(*this, outStream);
                    exit(0);
                    return false; },
                "[taixuan] Lists exact name of all midend passes.\n");
        registerOption("--parse-only", nullptr,
                [this](const char*) {
                    parseOnly = true;
                    return true; },
                "only parse the P4 input, without any further processing");
        registerOption("--validate", nullptr,
                [this](const char*) {
                    validateOnly = true;
                    return true;
                },
                "Validate the P4 input, running just the front-end");
        registerOption("--fromJSON", "file",
                [this](const char* arg) {
                    loadIRFromJson = true;
                    file = arg;
                    return true;
                },
                "read previously dumped json instead of P4 source code");
        registerOption("--pretty-print" , "file",
                [this](const char* arg) {
                    prettyPrint = true;
                    pp_file = arg;
                    return true;
                },
                "print the IR into P4 program");
        registerOption("--taixuan-remove-pragma" , "pragma1[,pragma2]",
                [this](const char* arg) {
                    pragmaModify = true;
                    auto copy = strdup(arg);
                    while (auto pass = strsep(&copy, ","))
                        removedFilters.push_back(pass);
                    return true;
                },
            "remove pragmas with specificed name");
        registerOption("--taixuan-insert-pragma", "type@location@pragma",
                [this](const char* arg) {
                    pragmaModify = true;
                    cstring c_arg = cstring(arg);
                    char delim[] = "@";
                    std::vector<cstring> params = c_arg.split(delim);
                    if (params.size() != 3) {
                        std::cerr << "Wrong input parameter: ";
                        std::cerr << c_arg << std::endl;
                        exit(0);
                        return false;
                    }
                    Taixuan::PragmaModify::IRTypes ir;
                    if (params[0] == "P")
                        ir = Taixuan::PragmaModify::PARSER;
                    else if (params[0] == "H")
                        ir = Taixuan::PragmaModify::HEADER;
                    else if (params[0] == "T")
                        ir = Taixuan::PragmaModify::TABLE;
                    else {
                        std::cerr << "Wrong pragma insert type: ";
                        std::cerr << c_arg << std::endl;
                        exit(0);
                        return false;
                    }
                    insertedAnnotations.push_back(
                        Taixuan::PragmaModify::PragmaOption(ir, params[1], params[2])
                    );
                    return true;
                },
                "insert pragma at desginated location");
        registerOption("--taixuan-pragma-output", "path",
                [this](const char* arg) {
                    pragmaOutputPath = arg;
                    return true;
                },
                "the output path of the program with modified pragmas");
     }
};

using TaixuanContext = P4CContextWithOptions<TaixuanOptions>;

static void log_dump(const IR::Node *node, const char *head) {
    if (node && LOGGING(1)) {
        if (head)
            std::cout << '+' << std::setw(strlen(head)+6) << std::setfill('-') << "+\n| "
                      << head << " |\n" << '+' << std::setw(strlen(head)+3) << "+" <<
                      std::endl << std::setfill(' ');
        if (LOGGING(2))
            dump(node);
        else
            std::cout << *node << std::endl; }
}

int main(int argc, char *const argv[]) {
    setup_gc_logging();
    setup_signals();

    AutoCompileContext autoTaixuanContext(new TaixuanContext);
    auto& options = TaixuanContext::get().options();
    options.langVersion = CompilerOptions::FrontendVersion::P4_16;
    options.compilerVersion = TAIXUAN_VERSION_STRING;

    if (options.process(argc, argv) != nullptr) {
            if (options.loadIRFromJson == false)
                    options.setInputFile();
    }
    if (::errorCount() > 0)
        return 1;
    const IR::P4Program *program = nullptr;
    auto hook = options.getDebugHook();
    if (options.loadIRFromJson) {
        std::ifstream json(options.file);
        if (json) {
            JSONLoader loader(json);
            const IR::Node* node = nullptr;
            loader >> node;
            if (!(program = node->to<IR::P4Program>()))
                error(ErrorType::ERR_INVALID, "%s is not a P4Program in json format", options.file);
        } else {
            error(ErrorType::ERR_IO, "Can't open %s", options.file); }
    } else {
        program = P4::parseP4File(options);

        if (program != nullptr && ::errorCount() == 0) {
            P4::P4COptionPragmaParser optionsPragmaParser;
            program->apply(P4::ApplyOptionsPragmas(optionsPragmaParser));

            /*
            if (!options.parseOnly) {
                try {
                    P4::FrontEnd fe;
                    fe.addDebugHook(hook);
                    program = fe.run(options, program);
                } catch (const std::exception &bug) {
                    std::cerr << bug.what() << std::endl;
                    return 1;
                }
            }
             */
        }
    }

    if (options.pragmaModify) {
        Taixuan::PragmaModify pragmaModify;
        // The sourcePath is the parent folder of the input program.
        cstring sourcePath = options.file.before(options.file.findlast('/'));
        pragmaModify.setDeleteFilters(options.removedFilters);
        pragmaModify.setInsertAnnotations(options.insertedAnnotations);
        pragmaModify.apply(program, sourcePath, options.pragmaOutputPath);
    }

    if (options.prettyPrint) {
        std::ostream *ppStream = openFile(options.pp_file, true);
        P4::ToP4 top4(ppStream, false);
        (void)program->apply(top4);
    }

    log_dump(program, "Initial program");
    if (program != nullptr && ::errorCount() == 0) {
        P4::serializeP4RuntimeIfRequired(program, options);

        if (!options.parseOnly && !options.validateOnly) {
            Taixuan::MidEnd midEnd(options);
            midEnd.addDebugHook(hook);
#if 0
            /* doing this breaks the output until we get dump/undump of srcInfo */
            if (options.debugJson) {
                std::stringstream tmp;
                JSONGenerator gen(tmp);
                gen << program;
                JSONLoader loader(tmp);
                loader >> program;
            }
#endif
            const IR::ToplevelBlock *top = nullptr;
            try {
                top = midEnd.process(program);
                // This can modify program!
                log_dump(program, "After midend");
                log_dump(top, "Top level block");
            } catch (const std::exception &bug) {
                std::cerr << bug.what() << std::endl;
                return 1;
            }
        }
        if (program) {
            if (options.dumpJsonFile)
                JSONGenerator(*openFile(options.dumpJsonFile, true), true) << program << std::endl;
            if (options.debugJson) {
                std::stringstream ss1, ss2;
                JSONGenerator gen1(ss1), gen2(ss2);
                gen1 << program;

                const IR::Node* node = nullptr;
                JSONLoader loader(ss1);
                loader >> node;

                gen2 << node;
                if (ss1.str() != ss2.str()) {
                    error(ErrorType::ERR_UNEXPECTED, "json mismatch");
                    std::ofstream t1("t1.json"), t2("t2.json");
                    t1 << ss1.str() << std::flush;
                    t2 << ss2.str() << std::flush;
                    auto rv = system("json_diff t1.json t2.json");
                    if (rv != 0) ::warning(ErrorType::WARN_FAILED,
                                           "json_diff failed with code %1%", rv);
                }
            }
        }
    }

    // demorgan();

    if (Log::verbose())
        std::cerr << "Done." << std::endl;
    return ::errorCount() > 0;
}
