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

#include "toNPL.h"

#include <deque>
#include <sstream>
#include <string>

#include "frontends/common/options.h"
#include "frontends/p4/fromv1.0/v1model.h"
#include "frontends/parsers/p4/p4parser.hpp"
#include "ir/dump.h"

namespace P4 {

Visitor::profile_t ToNPL::init_apply(const IR::Node *node) {
    LOG4("Program dump:" << std::endl << dumpToString(node));
    listTerminators_init_apply_size = listTerminators.size();
    vectorSeparator_init_apply_size = vectorSeparator.size();
    return Inspector::init_apply(node);
}

void ToNPL::end_apply(const IR::Node *) {
    if (outStream != nullptr) {
        cstring result = builder.toString();
        *outStream << result.c_str();
        outStream->flush();
    }
    BUG_CHECK(listTerminators.size() == listTerminators_init_apply_size,
              "inconsistent listTerminators");
    BUG_CHECK(vectorSeparator.size() == vectorSeparator_init_apply_size,
              "inconsistent vectorSeparator");
}

// Try to guess whether a file is a "system" file
bool ToNPL::isSystemFile(cstring file) {
    if (noIncludes) return false;
    if (file.startsWith(p4includePath)) return true;
    return false;
}

cstring ToNPL::ifSystemFile(const IR::Node *node) {
    if (!node->srcInfo.isValid()) return nullptr;
    auto sourceFile = node->srcInfo.getSourceFile();
    if (isSystemFile(sourceFile)) return sourceFile;
    return nullptr;
}

namespace {
class DumpIR : public Inspector {
    unsigned depth;
    std::stringstream str;

    DumpIR(unsigned depth, unsigned startDepth) : depth(depth) {
        for (unsigned i = 0; i < startDepth; i++) str << IndentCtl::indent;
        setName("DumpIR");
        visitDagOnce = false;
    }
    void display(const IR::Node *node) {
        str << IndentCtl::endl;
        if (node->is<IR::Member>()) {
            node->Node::dbprint(str);
            str << node->to<IR::Member>()->member;
        } else if (node->is<IR::Constant>()) {
            node->Node::dbprint(str);
            str << " " << node;
        } else if (node->is<IR::VectorBase>()) {
            node->Node::dbprint(str);
            str << ", size=" << node->to<IR::VectorBase>()->size();
        } else if (node->is<IR::Path>()) {
            node->dbprint(str);
        } else {
            node->Node::dbprint(str);
        }
    }

    bool goDeeper(const IR::Node *node) const {
        return node->is<IR::Expression>() || node->is<IR::Path>() || node->is<IR::Type>();
    }

    bool preorder(const IR::Node *node) override {
        if (depth == 0) return false;
        display(node);
        if (goDeeper(node))
            // increase depth limit for expressions.
            depth++;
        else
            depth--;
        str << IndentCtl::indent;
        return true;
    }
    void postorder(const IR::Node *node) override {
        if (goDeeper(node))
            depth--;
        else
            depth++;
        str << IndentCtl::unindent;
    }

 public:
    static std::string dump(const IR::Node *node, unsigned depth, unsigned startDepth) {
        DumpIR dumper(depth, startDepth);
        node->apply(dumper);
        auto result = dumper.str.str();
        return result;
    }
};
}  // namespace

void add(int &cnt) {
    cnt++;
}

void sub(int &cnt) {
    cnt--;
}

std::string build_string(int val) {
    return std::string(val, '\t');
}

unsigned ToNPL::curDepth() const {
    unsigned result = 0;
    auto ctx = getContext();
    while (ctx != nullptr) {
        ctx = ctx->parent;
        result++;
    }
    return result;
}

void ToNPL::dump(unsigned depth, const IR::Node *node, unsigned adjDepth) {
    if (!showIR) return;
    if (node == nullptr) node = getOriginal();

    auto str = DumpIR::dump(node, depth, adjDepth + curDepth());
    bool spc = builder.lastIsSpace();
    builder.commentStart();
    builder.append(str);
    builder.commentEnd();
    builder.newline();
    if (spc)
        // rather heuristic, but the output is very ugly anyway
        builder.emitIndent();
}

std::string generate_standard_metadata(std::map<std::string, std::string>& mp) {
    std::string ret_str = "struct standard_metadata_t {\n";
    ret_str += "\tfields {\n";
    for (auto &a : mp) {
        ret_str += "\t\t" + a.second + "\t" + a.first + "\n";
    }
    ret_str += "\t}\n";
    ret_str += "}\n";
    ret_str += "bus standard_metadata_t\t\tstandard_metadata;\n";
    return ret_str;
}

bool ToNPL::preorder(const IR::P4Program *program) {
    std::set<cstring> includesEmitted;

    bool first = true;
    dump(2);
    // NEW: count the total number of P4Control blocks
    for (auto a : program->objects) {
        if (a->is<IR::P4Control>()) {
            num_of_control_block++;
        }
    }
    for (auto a : program->objects) {
        // Check where this declaration originates
        cstring sourceFile = ifSystemFile(a);
        if (!a->is<IR::Type_Error>() &&  // errors can come from multiple files
            sourceFile != nullptr) {
            /* FIXME -- when including a user header file (sourceFile !=
             * mainFile), do we want to emit an #include of it or not?  Probably
             * not when translating from P4-14, as that would create a P4-16
             * file that tries to include a P4-14 header.  Unless we want to
             * allow converting headers independently (is that even possible?).
             * For now we ignore mainFile and don't emit #includes for any
             * non-system header */

            if (includesEmitted.find(sourceFile) == includesEmitted.end()) {
                if (sourceFile.startsWith(p4includePath)) {
                    const char *p = sourceFile.c_str() + strlen(p4includePath);
                    if (*p == '/') p++;
                    if (P4V1::V1Model::instance.file.name == p) {
                        P4V1::getV1ModelVersion g;
                        program->apply(g);
                        // ori: builder.append("#define V1MODEL_VERSION ");
                        // ori: builder.append(g.version);
                        // ori: builder.appendLine("");
                    }
                    // ori: builder.append("#include <");
                    // ori: builder.append(p);
                    // ori: builder.appendLine(">");
                } else {
                    // ori: builder.append("#include \"");
                    // ori: builder.append(sourceFile);
                    // ori: builder.appendLine("\"");
                }
                includesEmitted.emplace(sourceFile);
            }
            if (!isStandardmetadataPrint) {
                isStandardmetadataPrint = true;
                std::string standard_metadata_str = generate_standard_metadata(standard_metadata_mp);
                builder.append(standard_metadata_str);
            }
            first = false;
            continue;
        }
        if (!first) builder.newline();
        first = false;
        visit(a);
    }
    if (!program->objects.empty()) builder.newline();
    return false;
}

// example output: bit<8>
// DONE with the second half
bool ToNPL::preorder(const IR::Type_Bits *t) {
    std::cout << "Enter ToNPL::preorder(const IR::Type_Bits *t)" << t->toString() << std::endl;
    // std::cout << "cccccccccccccc" << t->toString() << std::endl;
    if (t->expression) {
        // ori: builder.append("bit<(");
        builder.append("bit[("); 
        visit(t->expression);
        // ori: builder.append(")>");
        builder.append(")]"); 
    } else {
        // turn bit<...> to bit[...]
        // TODO: find a better way to do such replace
        cstring curr_str = t->toString();
        curr_str = curr_str.replace('<', '[');
        curr_str = curr_str.replace('>', ']');
        // ori: builder.append(t->toString());
        builder.append(curr_str);
    }
    std::cout << "Exit ToNPL::preorder(const IR::Type_Bits *t)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Type_String *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_String *t)" << std::endl;
    builder.append(t->toString());
    return false;
}

bool ToNPL::preorder(const IR::Type_InfInt *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_InfInt *t)" << std::endl;
    builder.append(t->toString());
    return false;
}

bool ToNPL::preorder(const IR::Type_Var *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Var *t)" << std::endl;
    builder.append(t->name);
    return false;
}

bool ToNPL::preorder(const IR::Type_Unknown *) {
    BUG("Cannot emit code for an unknown type");
    // builder.append("*unknown type*");
    return false;
}

bool ToNPL::preorder(const IR::Type_Dontcare *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Dontcare *)" << std::endl;
    builder.append("_");
    return false;
}

bool ToNPL::preorder(const IR::Type_Void *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Void *)" << std::endl;
    builder.append("void");
    return false;
}

// example output: Type_Name *t = MyDeparser
bool ToNPL::preorder(const IR::Type_Name *t) {
    std::cout << "cccccccccccccc Enter ToNPL::preorder(const IR::Type_Name *t)" << t->toString() << std::endl;
    visit(t->path);
    std::cout << "cccccccccccccc Exit ToNPL::preorder(const IR::Type_Name *t)" << std::endl;
    return false;
}

// example output: *t = vlan_tag_t[2]
bool ToNPL::preorder(const IR::Type_Stack *t) {
    std::cout << "Enter ToNPL::preorder(const IR::Type_Stack *t)" << t->toString() << std::endl;
    dump(2);
    visit(t->elementType);
    // ori: builder.append("[");
    builder.append("[");
    visit(t->size);
    // ori: builder.append("]");
    builder.append("]");
    std::cout << "Exit ToNPL::preorder(const IR::Type_Stack *t)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Type_Specialized *t) {
    std::cout << "Enter ToNPL::preorder(const IR::Type_Specialized *t)" << t->toString() <<std::endl;
    // ori beg
    dump(3);
    visit(t->baseType);
    builder.append("<");
    setVecSep(", ");
    visit(t->arguments);
    doneVec();
    builder.append(">");
    // ori end
    std::cout << "Exit ToNPL::preorder(const IR::Type_Specialized *t)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Argument *arg) {
    std::cout << "Enter ToNPL::preorder(const IR::Argument *arg)" << arg->toString() << std::endl;
    dump(2);
    if (!arg->name.name.isNullOrEmpty()) {
        // ori: builder.append(arg->name.name);
        // ori: builder.append(" = ");
        builder.append(arg->name.name);
        builder.append(" = ");
    }
    visit(arg->expression);
    std::cout << "Exit ToNPL::preorder(const IR::Argument *arg)" << arg->toString() << std::endl;
    return false;
}


bool ToNPL::preorder(const IR::Type_Typedef *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Typedef *t)" << std::endl;
    dump(2);
    if (!t->annotations->annotations.empty()) {
        visit(t->annotations);
        builder.spc();
    }
    builder.append("typedef ");
    visit(t->type);
    builder.spc();
    builder.append(t->name);
    builder.endOfStatement();
    return false;
}

bool ToNPL::preorder(const IR::Type_Newtype *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Newtype *t)" << std::endl;
    dump(2);
    if (!t->annotations->annotations.empty()) {
        visit(t->annotations);
        builder.spc();
    }
    builder.append("type ");
    visit(t->type);
    builder.spc();
    builder.append(t->name);
    builder.endOfStatement();
    return false;
}

bool ToNPL::preorder(const IR::Type_BaseList *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_BaseList *t)" << std::endl;
    dump(3);
    builder.append("tuple<");
    bool first = true;
    for (auto a : t->components) {
        if (!first) builder.append(", ");
        first = false;
        auto p4type = a->getP4Type();
        CHECK_NULL(p4type);
        visit(p4type);
    }
    builder.append(">");
    return false;
}

bool ToNPL::preorder(const IR::P4ValueSet *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::P4ValueSet *t)" << std::endl;
    dump(1);
    if (!t->annotations->annotations.empty()) {
        visit(t->annotations);
        builder.spc();
    }
    builder.append("value_set<");
    auto p4type = t->elementType->getP4Type();
    CHECK_NULL(p4type);
    visit(p4type);
    builder.append(">");
    builder.append("(");
    visit(t->size);
    builder.append(")");
    builder.spc();
    builder.append(t->name);
    builder.endOfStatement();
    return false;
}

bool ToNPL::preorder(const IR::Type_Enum *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Enum *t)" << std::endl;
    dump(1);
    if (!t->annotations->annotations.empty()) {
        visit(t->annotations);
        builder.spc();
    }
    builder.append("enum ");
    builder.append(t->name);
    builder.spc();
    builder.blockStart();
    bool first = true;
    for (auto a : *t->getDeclarations()) {
        dump(2, a->getNode(), 1);
        if (!first) builder.append(",\n");
        first = false;
        builder.emitIndent();
        builder.append(a->getName());
    }
    builder.newline();
    builder.blockEnd(true);
    return false;
}

bool ToNPL::preorder(const IR::Type_SerEnum *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_SerEnum *t)" << std::endl;
    dump(1);
    if (!t->annotations->annotations.empty()) {
        visit(t->annotations);
        builder.spc();
    }
    builder.append("enum ");
    visit(t->type);
    builder.spc();
    builder.append(t->name);
    builder.spc();
    builder.blockStart();
    bool first = true;
    for (auto a : t->members) {
        dump(2, a->getNode(), 1);
        if (!first) builder.append(",\n");
        first = false;
        builder.emitIndent();
        builder.append(a->getName());
        builder.append(" = ");
        visit(a->value);
    }
    builder.newline();
    builder.blockEnd(true);
    return false;
}

bool ToNPL::preorder(const IR::TypeParameters *t) {
    std::cout << "Enter ToNPL::preorder(const IR::TypeParameters *t)" << t->toString() << std::endl;
    if (!t->empty()) {
        builder.append("<");
        bool first = true;
        bool decl = isDeclaration;
        isDeclaration = false;
        for (auto a : t->parameters) {
            if (!first) builder.append(", ");
            first = false;
            visit(a);
        }
        isDeclaration = decl;
        builder.append(">");
    }
    std::cout << "Exit ToNPL::preorder(const IR::TypeParameters *t)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Method *m) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Method *t)" << std::endl;
    dump(1);
    if (!m->annotations->annotations.empty()) {
        visit(m->annotations);
        builder.spc();
    }
    const Context *ctx = getContext();
    bool standaloneFunction = !ctx || !ctx->node->is<IR::Type_Extern>();
    // standalone function declaration: not in a Vector of methods
    if (standaloneFunction) builder.append("extern ");

    if (m->isAbstract) builder.append("abstract ");
    auto t = m->type;
    BUG_CHECK(t != nullptr, "Method %1% has no type", m);
    if (t->returnType != nullptr) {
        visit(t->returnType);
        builder.spc();
    }
    builder.append(m->name);
    visit(t->typeParameters);
    visit(t->parameters);
    if (standaloneFunction) builder.endOfStatement();
    return false;
}

bool ToNPL::preorder(const IR::Function *function) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Function *function)" << std::endl;
    dump(1);
    auto t = function->type;
    BUG_CHECK(t != nullptr, "Function %1% has no type", function);
    if (t->returnType != nullptr) {
        visit(t->returnType);
        builder.spc();
    }
    builder.append(function->name);
    visit(t->typeParameters);
    visit(t->parameters);
    builder.spc();
    visit(function->body);
    return false;
}

bool ToNPL::preorder(const IR::Type_Extern *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Extern *t)" << std::endl;
    dump(2);
    if (isDeclaration) {
        if (!t->annotations->annotations.empty()) {
            visit(t->annotations);
            builder.spc();
        }
        builder.append("extern ");
    }
    builder.append(t->name);
    visit(t->typeParameters);
    if (!isDeclaration) return false;
    builder.spc();
    builder.blockStart();

    if (t->attributes.size() != 0)
        warn(ErrorType::WARN_UNSUPPORTED,
             "%1%: extern has attributes, which are not supported "
             "in P4-16, and thus are not emitted as P4-16",
             t);

    setVecSep(";\n", ";\n");
    bool decl = isDeclaration;
    isDeclaration = true;
    preorder(&t->methods);
    isDeclaration = decl;
    doneVec();
    builder.blockEnd(true);
    return false;
}

bool ToNPL::preorder(const IR::Type_Boolean *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Boolean *)" << std::endl;
    builder.append("bool");
    return false;
}

bool ToNPL::preorder(const IR::Type_Varbits *t) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Varbits *t)" << std::endl;
    if (t->expression) {
        builder.append("varbit<(");
        visit(t->expression);
        builder.append(")>");
    } else {
        builder.appendFormat("varbit<%d>", t->size);
    }
    return false;
}

bool ToNPL::preorder(const IR::Type_Package *package) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Type_Package *package)" << std::endl;
    dump(2);
    builder.emitIndent();
    if (!package->annotations->annotations.empty()) {
        visit(package->annotations);
        builder.spc();
    }
    builder.append("package ");
    builder.append(package->name);
    visit(package->typeParameters);
    visit(package->constructorParams);
    if (isDeclaration) builder.endOfStatement();
    return false;
}

bool ToNPL::process(const IR::Type_StructLike *t, const char *name) {
    std::cout << "Enter ToNPL::process(const IR::Type_StructLike *t, const char *name)" << t->toString() << "name =" << name << std::endl;
    dump(2);
    if (isDeclaration) {
        builder.emitIndent();
        if (!t->annotations->annotations.empty()) {
            visit(t->annotations);
            builder.spc();
        }
        // ori: builder.appendFormat("%s ", name);
        builder.appendFormat("%s ", "struct"); // NEW in NPL, there are no differences between struct and header
    }
    builder.append(t->name);
    visit(t->typeParameters);
    if (!isDeclaration) return false;
    builder.spc();
    builder.blockStart();

    builder.append("\tfields"); // NEW
    builder.blockStart(); // NEW

    std::map<const IR::StructField *, cstring> type;
    size_t len = 0;
    for (auto f : t->fields) {
        Util::SourceCodeBuilder builder;
        ToNPL rec(builder, showIR);

        f->type->apply(rec);
        cstring t = builder.toString();
        if (t.size() > len) len = t.size();
        type.emplace(f, t);
    }

    for (auto f : t->fields) {
        dump(4, f, 1);  // this will dump annotations
        if (f->annotations->size() > 0) {
            builder.emitIndent();
            if (!f->annotations->annotations.empty()) {
                visit(f->annotations);
            }
            builder.newline();
        }
        builder.emitIndent();
        cstring t = get(type, f);
        builder.append(t);
        size_t spaces = len + 1 - t.size();
        builder.append(std::string(spaces, ' '));
        builder.append(f->name);
        builder.endOfStatement(true);
    }
    builder.blockEnd(true); // NEW
    builder.blockEnd(true);
    // NEW define either bus of 
    std::cout << "Exit ToNPL::process(const IR::Type_StructLike *t, const char *name)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Type_Parser *t) {
    std::cout << "Enter ToNPL::preorder(const IR::Type_Parser *t)" << t->toString() << std::endl;
    if (!startParser) {
        builder.append("parser_node start "); // NEW
        builder.blockStart(); // NEW
        builder.append("\t root_node : 1;\n"); // NEW
        builder.append("\t end_node : 1;\n"); // NEW
        builder.blockEnd(true); // NEW
        startParser = true;
    }
    dump(2);
    // Assume only one parse, would need to write the parser again (TODO: if there is logic of parsing)
    // ori begin
    // builder.emitIndent();
    // if (!t->annotations->annotations.empty()) {
    //     visit(t->annotations);
    //     builder.spc();
    // }
    // builder.append("parser ");
    // builder.append(t->name);
    // visit(t->typeParameters);
    // visit(t->applyParams);
    // if (isDeclaration) builder.endOfStatement();
    // ori end
    std::cout << "Exit ToNPL::preorder(const IR::Type_Parser *t)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Type_Control *t) {
    std::cout << "ToNPL::preorder(const IR::Type_Control *t)" << std::endl;
    dump(2);
    builder.emitIndent();
    if (!t->annotations->annotations.empty()) {
        visit(t->annotations);
        builder.spc();
    }
    builder.append("control ");
    builder.append(t->name);
    visit(t->typeParameters);
    visit(t->applyParams);
    if (isDeclaration) builder.endOfStatement();
    return false;
}

///////////////////////
// DONE constant is easy to update
bool ToNPL::preorder(const IR::Constant *c) {
    std::cout << "Enter ToNPL::preorder(const IR::Constant *c)" << c->toString() << std::endl;
    const IR::Type_Bits *tb = dynamic_cast<const IR::Type_Bits *>(c->type);
    unsigned width;
    bool sign;
    if (tb == nullptr) {
        width = 0;
        sign = false;
    } else {
        width = tb->size;
        sign = tb->isSigned;
    }
    cstring s = Util::toString(c->value, width, sign, c->base);
    // ori: builder.append(s);
    builder.append(s);
    std::cout << "Exit ToNPL::preorder(const IR::Constant *c)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::BoolLiteral *b) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::BoolLiteral *b)" << std::endl;
    builder.append(b->toString());
    return false;
}

bool ToNPL::preorder(const IR::StringLiteral *s) {
    std::cout << "Enter ToNPL::preorder(const IR::StringLiteral *s)" << s->toString() << std::endl;
    builder.append(s->toString());
    std::cout << "Exit ToNPL::preorder(const IR::StringLiteral *s)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Declaration_Constant *cst) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Declaration_Constant *cst)" << std::endl;
    dump(2);
    if (!cst->annotations->annotations.empty()) {
        visit(cst->annotations);
        builder.spc();
    }
    builder.append("const ");
    auto type = cst->type->getP4Type();
    CHECK_NULL(type);
    visit(type);
    builder.spc();
    builder.append(cst->name);
    builder.append(" = ");

    setListTerm("{ ", " }");
    visit(cst->initializer);
    doneList();

    builder.endOfStatement();
    return false;
}

bool ToNPL::preorder(const IR::Declaration_Instance *i) {
    std::cout << "Enter ToNPL::preorder(const IR::Declaration_Instance *i)" << i->toString() << std::endl;
    // ori beg
    // dump(3);
    // if (!i->annotations->annotations.empty()) {
    //     visit(i->annotations);
    //     builder.spc();
    // }
    // auto type = i->type->getP4Type();
    // CHECK_NULL(type);
    // visit(type);
    // builder.append("(");
    // setVecSep(", ");
    // visit(i->arguments);
    // doneVec();
    // builder.append(")");
    // builder.spc();
    // builder.append(i->name);
    // if (i->initializer != nullptr) {
    //     builder.append(" = ");
    //     visit(i->initializer);
    // }
    // builder.endOfStatement();
    // ori end
    std::cout << "Exit ToNPL::preorder(const IR::Declaration_Instance *i)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Declaration_Variable *v) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Declaration_Variable *v)" << std::endl;
    dump(2);
    if (!v->annotations->annotations.empty()) {
        visit(v->annotations);
        builder.spc();
    }
    auto type = v->type->getP4Type();
    CHECK_NULL(type);
    visit(type);
    builder.spc();
    builder.append(v->name);
    if (v->initializer != nullptr) {
        builder.append(" = ");
        setListTerm("{ ", " }");
        visit(v->initializer);
        doneList();
    }
    builder.endOfStatement();
    return false;
}

// 先不用管
bool ToNPL::preorder(const IR::Type_Error *d) {
    std::cout << "Enter ToNPL::preorder(const IR::Type_Error *d)" << d->toString() << std::endl;
    dump(1);
    bool first = true;
    for (auto a : *d->getDeclarations()) {
        if (ifSystemFile(a->getNode()))
            // only print if not from a system file
            continue;
        if (!first) {
            builder.append(",\n");
        } else {
            builder.append("error ");
            builder.blockStart();
        }
        dump(1, a->getNode(), 1);
        first = false;
        builder.emitIndent();
        builder.append(a->getName());
    }
    if (!first) {
        builder.newline();
        builder.blockEnd(true);
    }
    std::cout << "Exit ToNPL::preorder(const IR::Type_Error *d)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Declaration_MatchKind *d) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Declaration_MatchKind *d)" << std::endl;
    dump(1);
    builder.append("match_kind ");
    builder.blockStart();
    bool first = true;
    for (auto a : *d->getDeclarations()) {
        if (!first) builder.append(",\n");
        dump(1, a->getNode(), 1);
        first = false;
        builder.emitIndent();
        builder.append(a->getName());
    }
    builder.newline();
    builder.blockEnd(true);
    return false;
}

///////////////////////////////////////////////////

#define VECTOR_VISIT(V, T)                                    \
    bool ToNPL::preorder(const IR::V<IR::T> *v) {              \
        if (v == nullptr) return false;                       \
        bool first = true;                                    \
        VecPrint sep = getSep();                              \
        for (auto a : *v) {                                   \
            if (!first) {                                     \
                builder.append(sep.separator);                \
            }                                                 \
            if (sep.separator.endsWith("\n")) {               \
                builder.emitIndent();                         \
            }                                                 \
            first = false;                                    \
            visit(a);                                         \
        }                                                     \
        if (!v->empty() && !sep.terminator.isNullOrEmpty()) { \
            builder.append(sep.terminator);                   \
        }                                                     \
        return false;                                         \
    }

VECTOR_VISIT(Vector, ActionListElement)
VECTOR_VISIT(Vector, Annotation)
VECTOR_VISIT(Vector, Entry)
VECTOR_VISIT(Vector, Expression)
VECTOR_VISIT(Vector, Argument)
VECTOR_VISIT(Vector, KeyElement)
VECTOR_VISIT(Vector, Method)
VECTOR_VISIT(Vector, Node)
VECTOR_VISIT(Vector, SelectCase)
VECTOR_VISIT(Vector, SwitchCase)
VECTOR_VISIT(Vector, Type)
VECTOR_VISIT(IndexedVector, Declaration)
VECTOR_VISIT(IndexedVector, Declaration_ID)
VECTOR_VISIT(IndexedVector, Node)
VECTOR_VISIT(IndexedVector, ParserState)
VECTOR_VISIT(IndexedVector, StatOrDecl)

#undef VECTOR_VISIT

///////////////////////////////////////////

bool ToNPL::preorder(const IR::Slice *slice) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Slice *e)" << std::endl;
    int prec = expressionPrecedence;
    bool useParens = prec > slice->getPrecedence();
    if (useParens) builder.append("(");
    expressionPrecedence = slice->getPrecedence();

    visit(slice->e0);
    builder.append("[");
    expressionPrecedence = DBPrint::Prec_Low;
    visit(slice->e1);
    builder.append(":");
    expressionPrecedence = DBPrint::Prec_Low;
    visit(slice->e2);
    builder.append("]");
    expressionPrecedence = prec;

    if (useParens) builder.append(")");
    return false;
}

bool ToNPL::preorder(const IR::DefaultExpression *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::DefaultExpression *e)" << std::endl;
    // Within a method call this is rendered as a don't care
    if (withinArgument)
        builder.append("_");
    else
        builder.append("default");
    return false;
}

bool ToNPL::preorder(const IR::This *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::This *e)" << std::endl;
    builder.append("this");
    return false;
}

bool ToNPL::preorder(const IR::PathExpression *p) {
    std::cout << "Enter ToNPL::preorder(const IR::PathExpression *e)" << p->toString() << std::endl;
    // Ignore NoAction, TODO: find a better way to ignore
    if (p->toString().find("NoAction")) {
        std::cout << "Early Exit ToNPL::preorder(const IR::PathExpression *e)" << std::endl;
        return false;
    }
    visit(p->path);
    std::cout << "Exit ToNPL::preorder(const IR::PathExpression *e)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::TypeNameExpression *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::TypeNameExpression *e)" << std::endl;
    visit(e->typeName);
    return false;
}

bool ToNPL::preorder(const IR::ConstructorCallExpression *e) {
    std::cout << "Enter ToNPL::preorder(const IR::ConstructorCallExpression *e)" << e->toString() << std::endl;
    visit(e->constructedType);
    builder.append("(");
    setVecSep(", ");
    int prec = expressionPrecedence;
    expressionPrecedence = DBPrint::Prec_Low;
    visit(e->arguments);
    expressionPrecedence = prec;
    doneVec();
    builder.append(")");
    std::cout << "Exit ToNPL::preorder(const IR::ConstructorCallExpression *e)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Member *e) {
    std::cout << "Enter ToNPL::preorder(const IR::Member *e)" << e->toString() << std::endl;
    int prec = expressionPrecedence;
    expressionPrecedence = e->getPrecedence();
    visit(e->expr);
    builder.append(".");
    std::cout << "e->member = " << e->member << std::endl;
    if (e->member == "apply") {
        // Assume we would look up at most once per table
        // TODO: consider other scenarios
        builder.append("lookup(0)"); // NEW
        isapply = true;
    } else {
        builder.append(e->member);
        isapply = false;
    }
    expressionPrecedence = prec;
    std::cout << "Exit ToNPL::preorder(const IR::Member *e)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::SelectCase *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::SelectCase *e)" << std::endl;
    dump(2);
    int prec = expressionPrecedence;
    expressionPrecedence = DBPrint::Prec_Low;
    setListTerm("(", ")");
    visit(e->keyset);
    expressionPrecedence = prec;
    doneList();
    builder.append(": ");
    visit(e->state);
    return false;
}

bool ToNPL::preorder(const IR::SelectExpression *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::SelectExpression *e)" << std::endl;
    builder.append("select(");
    int prec = expressionPrecedence;
    expressionPrecedence = DBPrint::Prec_Low;
    setListTerm("", "");
    visit(e->select);
    doneList();
    builder.append(") ");
    builder.blockStart();
    setVecSep(";\n", ";\n");
    expressionPrecedence = DBPrint::Prec_Low;
    preorder(&e->selectCases);
    doneVec();
    builder.blockEnd(true);
    expressionPrecedence = prec;
    return false;
}

bool ToNPL::preorder(const IR::ListExpression *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::ListExpression *e)" << std::endl;
    cstring start, end;
    if (listTerminators.empty()) {
        start = "{ ";
        end = " }";
    } else {
        start = listTerminators.back().start;
        end = listTerminators.back().end;
    }
    builder.append(start);
    setVecSep(", ");
    int prec = expressionPrecedence;
    expressionPrecedence = DBPrint::Prec_Low;
    setListTerm("{ ", " }");
    preorder(&e->components);
    doneList();
    expressionPrecedence = prec;
    doneVec();
    builder.append(end);
    return false;
}

bool ToNPL::preorder(const IR::P4ListExpression *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::P4ListExpression *e)" << std::endl;
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append("(");
    if (e->elementType != nullptr) {
        builder.append("(list<");
        visit(e->elementType->getP4Type());
        builder.append(">)");
    }
    builder.append("{");
    int prec = expressionPrecedence;
    expressionPrecedence = DBPrint::Prec_Low;
    bool first = true;
    for (auto c : e->components) {
        if (!first) builder.append(",");
        first = false;
        visit(c);
    }
    expressionPrecedence = prec;
    builder.append("}");
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append(")");
    return false;
}

bool ToNPL::preorder(const IR::NamedExpression *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::NamedExpression *e)" << std::endl;
    builder.append(e->name.name);
    builder.append(" = ");
    visit(e->expression);
    return false;
}

bool ToNPL::preorder(const IR::StructExpression *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::StructExpression *e)" << std::endl;
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append("(");
    if (e->structType != nullptr) {
        builder.append("(");
        visit(e->structType);
        builder.append(")");
    }
    builder.append("{");
    int prec = expressionPrecedence;
    expressionPrecedence = DBPrint::Prec_Low;
    bool first = true;
    for (auto c : e->components) {
        if (!first) builder.append(",");
        first = false;
        visit(c);
    }
    expressionPrecedence = prec;
    builder.append("}");
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append(")");
    return false;
}

bool ToNPL::preorder(const IR::HeaderStackExpression *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::HeaderStackExpression *e)" << std::endl;
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append("(");
    if (e->headerStackType != nullptr) {
        builder.append("(");
        visit(e->headerStackType);
        builder.append(")");
    }
    builder.append("{");
    int prec = expressionPrecedence;
    expressionPrecedence = DBPrint::Prec_Low;
    bool first = true;
    for (auto c : e->components) {
        if (!first) builder.append(",");
        first = false;
        visit(c);
    }
    expressionPrecedence = prec;
    builder.append("}");
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append(")");
    return false;
}

bool ToNPL::preorder(const IR::Invalid *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Invalid *e)" << std::endl;
    builder.append("{#}");
    return false;
}

bool ToNPL::preorder(const IR::Dots *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Dots *e)" << std::endl;
    builder.append("...");
    return false;
}

bool ToNPL::preorder(const IR::NamedDots *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::NamedDots *e)" << std::endl;
    builder.append("...");
    return false;
}

bool ToNPL::preorder(const IR::InvalidHeader *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::InvalidHeader *e)" << std::endl;
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append("(");
    builder.append("(");
    visit(e->headerType);
    builder.append(")");
    builder.append("{#}");
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append(")");
    return false;
}

bool ToNPL::preorder(const IR::InvalidHeaderUnion *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::InvalidHeaderUnion *e)" << std::endl;
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append("(");
    builder.append("(");
    visit(e->headerUnionType);
    builder.append(")");
    builder.append("{#}");
    if (expressionPrecedence > DBPrint::Prec_Prefix) builder.append(")");
    return false;
}

bool ToNPL::preorder(const IR::MethodCallExpression *e) {
    std::cout << "Enter ToNPL::preorder(const IR::MethodCallExpression *e)" << e->toString() << std::endl;
    // Ignore NoAction, TODO: find a better way to ignore
    if (e->toString().find("NoAction")) {
        std::cout << "Early Exit ToNPL::preorder(const IR::MethodCallExpression *e)" << std::endl;
        return false;
    }
    int prec = expressionPrecedence;
    bool useParens = (prec > DBPrint::Prec_Postfix) ||
                     (!e->typeArguments->empty() && prec >= DBPrint::Prec_Cond);
    // FIXME: we use parenthesis more often than necessary
    // because the bison parser has a bug which parses
    // these expressions incorrectly.
    expressionPrecedence = DBPrint::Prec_Postfix;
    if (useParens) builder.append("(");
    visit(e->method);
    if (!e->typeArguments->empty()) {
        bool decl = isDeclaration;
        isDeclaration = false;
        builder.append("<");
        setVecSep(", ");
        visit(e->typeArguments);
        doneVec();
        builder.append(">");
        isDeclaration = decl;
    }
    // ori beg
    // builder.append("(");
    // setVecSep(", ");
    // expressionPrecedence = DBPrint::Prec_Low;
    // withinArgument = true;
    // visit(e->arguments);
    // withinArgument = false;
    // doneVec();
    // builder.append(")");
    // if (useParens) builder.append(")");
    // expressionPrecedence = prec;
    // ori end
    if (isapply == false) {
        builder.append("(");
        setVecSep(", ");
        expressionPrecedence = DBPrint::Prec_Low;
        withinArgument = true;
        visit(e->arguments);
        withinArgument = false;
        doneVec();
        builder.append(")");
        if (useParens) builder.append(")");
        expressionPrecedence = prec;
    }
    std::cout << "Exit ToNPL::preorder(const IR::MethodCallExpression *e)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Operation_Binary *b) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Operation_Binary *b)" << std::endl;
    int prec = expressionPrecedence;
    bool useParens = prec > b->getPrecedence();
    if (useParens) builder.append("(");
    expressionPrecedence = b->getPrecedence();
    visit(b->left);
    builder.spc();
    builder.append(b->getStringOp());
    builder.spc();
    expressionPrecedence = b->getPrecedence() + 1;
    visit(b->right);
    if (useParens) builder.append(")");
    expressionPrecedence = prec;
    return false;
}

bool ToNPL::preorder(const IR::Mux *b) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Mux *b)" << std::endl;
    int prec = expressionPrecedence;
    bool useParens = prec >= b->getPrecedence();
    if (useParens) builder.append("(");
    expressionPrecedence = b->getPrecedence();
    visit(b->e0);
    builder.append(" ? ");
    expressionPrecedence = DBPrint::Prec_Low;
    visit(b->e1);
    builder.append(" : ");
    expressionPrecedence = b->getPrecedence();
    visit(b->e2);
    expressionPrecedence = prec;
    if (useParens) builder.append(")");
    return false;
}

bool ToNPL::preorder(const IR::Operation_Unary *u) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Operation_Unary *u)" << std::endl;
    int prec = expressionPrecedence;
    bool useParens = prec > u->getPrecedence();
    if (useParens) builder.append("(");
    builder.append(u->getStringOp());
    expressionPrecedence = u->getPrecedence();
    visit(u->expr);
    expressionPrecedence = prec;
    if (useParens) builder.append(")");
    return false;
}

bool ToNPL::preorder(const IR::ArrayIndex *a) {
    std::cout << "Enter ToNPL::preorder(const IR::ArrayIndex *a)" << a->toString() << std::endl;
    int prec = expressionPrecedence;
    bool useParens = prec > a->getPrecedence();
    if (useParens) builder.append("(");
    expressionPrecedence = a->getPrecedence();
    visit(a->left);
    builder.append("[");
    expressionPrecedence = DBPrint::Prec_Low;
    visit(a->right);
    builder.append("]");
    if (useParens) builder.append(")");
    expressionPrecedence = prec;
    std::cout << "Exit ToNPL::preorder(const IR::ArrayIndex *a)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Cast *c) {
    std::cout << "这里暂时应该到不了  ToNPL::preorder(const IR::Cast *c)" << std::endl;
    int prec = expressionPrecedence;
    bool useParens = prec > c->getPrecedence();
    if (useParens) builder.append("(");
    builder.append("(");
    visit(c->destType);
    builder.append(")");
    expressionPrecedence = c->getPrecedence();
    visit(c->expr);
    if (useParens) builder.append(")");
    expressionPrecedence = prec;
    return false;
}

//////////////////////////////////////////////////////////

bool ToNPL::preorder(const IR::AssignmentStatement *a) {
    std::cout << "Enter ToNPL::preorder(const IR::AssignmentStatement *a)" << std::endl;
    // Util::SourceCodeBuilder &tmp_builder = *new Util::SourceCodeBuilder();
    dump(2);
    visit(a->left);
    builder.append(" = ");
    visit(a->right);
    builder.endOfStatement();
    std::cout << "Exit ToNPL::preorder(const IR::AssignmentStatement *a)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::BlockStatement *s) {
    std::cout << "Enter ToNPL::preorder(const IR::BlockStatement *s)" << s->toString() << std::endl;
    dump(1);
    if (!s->annotations->annotations.empty()) {
        visit(s->annotations);
        builder.spc();
    }
    // NEW add one { only when reaching the first control block
    if (firstControlBlock) {
        builder.blockStart();    
        firstControlBlock = false;
    }
    setVecSep("\n", "\n");
    preorder(&s->components);
    doneVec();
    // NEW close using { when finishing parsing the last control block
    if (num_of_control_block == curr_control_block_num) {
        builder.blockEnd(false);
    }
    std::cout << "Exit ToNPL::preorder(const IR::BlockStatement *s)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::ExitStatement *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::ExitStatement *)" << std::endl;
    dump(1);
    builder.append("exit");
    builder.endOfStatement();
    return false;
}

bool ToNPL::preorder(const IR::ReturnStatement *statement) {
    dump(2);
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::ReturnStatement *statement)" << std::endl;
    builder.append("return");
    if (statement->expression != nullptr) {
        builder.spc();
        visit(statement->expression);
    }
    builder.endOfStatement();
    return false;
}

// Need change (DONE)
bool ToNPL::preorder(const IR::EmptyStatement *) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::EmptyStatement *)" << std::endl;
    dump(1);
    // builder.endOfStatement();
    builder.endOfStatement(); // change
    return false;
}

// Need change
bool ToNPL::preorder(const IR::IfStatement *s) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::IfStatement *)" << std::endl;
    dump(2);
    builder.append("if (");
    visit(s->condition);
    builder.append(") ");
    if (!s->ifTrue->is<IR::BlockStatement>()) {
        builder.append("{");
        builder.increaseIndent();
        builder.newline();
        builder.emitIndent();
    }
    visit(s->ifTrue);
    if (!s->ifTrue->is<IR::BlockStatement>()) {
        builder.newline();
        builder.decreaseIndent();
        builder.emitIndent();
        builder.append("}");
    }
    if (s->ifFalse != nullptr) {
        builder.append(" else ");
        if (!s->ifFalse->is<IR::BlockStatement>() && !s->ifFalse->is<IR::IfStatement>()) {
            builder.append("{");
            builder.increaseIndent();
            builder.newline();
            builder.emitIndent();
        }
        visit(s->ifFalse);
        if (!s->ifFalse->is<IR::BlockStatement>() && !s->ifFalse->is<IR::IfStatement>()) {
            builder.newline();
            builder.decreaseIndent();
            builder.emitIndent();
            builder.append("}");
        }
    }
    return false;
}

bool ToNPL::preorder(const IR::MethodCallStatement *s) {
    std::cout << "Enter ToNPL::preorder(const IR::MethodCallStatement *s)" << s->toString() << std::endl;
    dump(3);
    visit(s->methodCall);
    builder.endOfStatement();
    std::cout << "Exit ToNPL::preorder(const IR::MethodCallStatement *s)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::SwitchCase *s) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::SwitchCase *s)" << std::endl;
    visit(s->label);
    builder.append(": ");
    visit(s->statement);
    return false;
}

bool ToNPL::preorder(const IR::SwitchStatement *s) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::SwitchStatement *s)" << std::endl;
    dump(4);
    builder.append("switch (");
    visit(s->expression);
    builder.append(") ");
    builder.blockStart();
    setVecSep("\n", "\n");
    preorder(&s->cases);
    doneVec();
    builder.blockEnd(false);
    return false;
}

////////////////////////////////////

bool ToNPL::preorder(const IR::Annotations *a) {
    std::cout << "Enter ToNPL::preorder(const IR::Annotations *a)" << a->toString() << std::endl;
    bool first = true;
    for (const auto *anno : a->annotations) {
        if (!first) {
            builder.spc();
        } else {
            first = false;
        }
        visit(anno);
    }
    std::cout << "Exit ToNPL::preorder(const IR::Annotations *a)" << std::endl;
    return false;
}

// No need to record the annotation
bool ToNPL::preorder(const IR::Annotation *a) {
    std::cout << "Enter ToNPL::preorder(const IR::Annotation *a)" << a->toString() << std::endl;
    /*
    builder.append("@");
    builder.append(a->name);
    char open = a->structured ? '[' : '(';
    char close = a->structured ? ']' : ')';
    if (!a->expr.empty()) {
        builder.append(open);
        setVecSep(", ");
        preorder(&a->expr);
        doneVec();
        builder.append(close);
    }
    if (!a->kv.empty()) {
        builder.append(open);
        bool first = true;
        for (auto kvp : a->kv) {
            if (!first) builder.append(", ");
            first = false;
            builder.append(kvp->name);
            builder.append("=");
            visit(kvp->expression);
        }
        builder.append(close);
    }
    if (a->expr.empty() && a->kv.empty() && a->structured) {
        builder.append("[]");
    }
    if (!a->body.empty() && a->expr.empty() && a->kv.empty()) {
        // Have an unparsed annotation.
        // We could be prettier here with smarter logic, but let's do the easy
        // thing by separating every token with a space.
        builder.append(open);
        bool first = true;
        for (auto tok : a->body) {
            if (!first) builder.append(" ");
            first = false;

            bool haveStringLiteral = tok->token_type == P4Parser::token_type::TOK_STRING_LITERAL;
            if (haveStringLiteral) builder.append("\"");
            builder.append(tok->text);
            if (haveStringLiteral) builder.append("\"");
        }
        builder.append(close);
    }
    */
   std::cout << "Exit ToNPL::preorder(const IR::Annotation *a)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::Parameter *p) {
    std::cout << "Enter ToNPL::preorder(const IR::Parameter *p)" << p->toString() << std::endl;
    dump(2);
    if (!p->annotations->annotations.empty()) {
        visit(p->annotations);
        builder.spc();
    }
    switch (p->direction) {
        case IR::Direction::None:
            break;
        case IR::Direction::In:
            builder.append("in ");
            break;
        case IR::Direction::Out:
            builder.append("out ");
            break;
        case IR::Direction::InOut:
            builder.append("inout ");
            break;
        default:
            BUG("Unexpected case");
    }
    bool decl = isDeclaration;
    isDeclaration = false;
    visit(p->type);
    isDeclaration = decl;
    builder.spc();
    builder.append(p->name);
    if (p->defaultValue != nullptr) {
        builder.append("=");
        visit(p->defaultValue);
    }
    std::cout << "Exit ToNPL::preorder(const IR::Parameter *p)" << p->toString() << std::endl;
    return false;
}



// Definitely need change
bool ToNPL::preorder(const IR::P4Control *c) {
    add(count);
    std::cout << build_string(count);
    // std::cout << "Enter ToNPL::preorder(const IR::P4Control *c)" << c->name << std::endl;  // Control name
    std::cout << "Enter ToNPL::preorder(const IR::P4Control *c)" << c->toString() << std::endl;
    curr_control_block_num++;
    dump(1);
    // ori beg
    // bool decl = isDeclaration;
    // isDeclaration = false;
    // visit(c->type);
    // isDeclaration = decl;
    // if (c->constructorParams->size() != 0) visit(c->constructorParams);
    // builder.spc();
    // builder.blockStart();
    // for (auto s : c->controlLocals) {
    //     builder.emitIndent();
    //     visit(s);
    //     builder.newline();
    // }

    // builder.emitIndent();
    // builder.append("apply ");
    // ori end

    // NEW beg
    
    for (auto s : c->controlLocals) {
        builder.emitIndent();
        visit(s);
        builder.newline();
    }
    builder.emitIndent();
    if (firstControlBlock) {
        builder.append("program ");
        builder.append(c->getName());
    }
    
    builder.spc();
    // ori: builder.blockStart();
    // NEW end
    visit(c->body);
    // ori: builder.newline();
    // ori: builder.blockEnd(true);
    std::cout << build_string(count);
    sub(count);
    std::cout << "Exit ToNPL::preorder(const IR::P4Control *c)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::ParameterList *p) {
    std::cout << "Enter ToNPL::preorder(const IR::ParameterList *p)" << p->toString() << std::endl;
    builder.append("(");
    bool first = true;
    for (auto param : *p->getEnumerator()) {
        if (!first) builder.append(", ");
        first = false;
        visit(param);
    }
    builder.append(")");
    std::cout << "Exit ToNPL::preorder(const IR::ParameterList *p)" << std::endl;
    return false;
}

void update_para_map(std::map<cstring, std::string> &para_mp, const IR::P4Action *c, bool showIR) {
    if (c->parameters) {
        for (auto &a : c->parameters->parameters) {
            std::string key_str = a->toString().c_str();
            Util::SourceCodeBuilder tmp_builder;
            ToNPL rec(tmp_builder, showIR);
            a->type->apply(rec);
            std::string val_str = tmp_builder.toString().c_str();
            para_mp[key_str] = val_str;
        }
    }
}

bool ToNPL::preorder(const IR::P4Action *c) {
    std::cout << "Enter ToNPL::preorder(const IR::P4Action *c)" << c->toString() << std::endl;
    // Ignore NoAction TODO: find a better way to deal with it
    if (c->toString().find("NoAction")) {
        std::cout << "Early Exit ToNPL::preorder(const IR::P4Action *c)" << std::endl;
        return false;
    }
    dump(2);
    if (!c->annotations->annotations.empty()) {
        visit(c->annotations);
        builder.spc();
    }

    // Update the action_para_map
    action_para_map[c->name] = {};
    update_para_map(action_para_map[c->name], c, showIR);
    // for (auto &a : action_para_map) {
    //     std::cout << "a.first = " << a.first << std::endl;
    //     for (auto &v : a.second) {
    //         std::cout << "v.first = " << v.first << " v.second = " << v.second << std::endl;
    //     }
    // }

    // ori: builder.append("action ");
    // ori: builder.append(c->name);
    // ori: visit(c->parameters);
    // ori: builder.spc();
    // ori: visit(c->body);
    // Assume one action per table; TODO: support multiple actions
    // builder.append("fields_assign() "); // NEW
    // builder.blockStart(); // NEW
    // builder.append("\t\tif (_LOOKUP0)"); // NEW
    // visit(c->body); // NEW
    // builder.blockEnd(true); // NEW
    // NEW start:
    Util::SourceCodeBuilder tmp_builder;
    ToNPL rec(tmp_builder, showIR);
    c->body->apply(rec);
    cstring s = tmp_builder.toString();
    s = "\tfields_assign() { \n\t\tif (_LOOKUP0 == 1) " + s;
    s += " \n\t}";
    // std::cout << "--------------s = " << s << std::endl;
    std::string new_string = s.c_str();
    action_map[c->name] = new_string;
    // NEW end
        
    std::cout << "Exit ToNPL::preorder(const IR::P4Action *c)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::ParserState *s) {
    std::cout << "ToNPL::preorder(const IR::ParserState *s)" << std::endl;
    dump(1);
    if (s->isBuiltin()) return false;

    if (!s->annotations->annotations.empty()) {
        visit(s->annotations);
        builder.spc();
    }
    builder.append("state ");
    builder.append(s->name);
    builder.spc();
    builder.blockStart();
    setVecSep("\n", "\n");
    preorder(&s->components);
    doneVec();

    if (s->selectExpression != nullptr) {
        dump(2, s->selectExpression, 1);
        builder.emitIndent();
        builder.append("transition ");
        visit(s->selectExpression);
        if (!s->selectExpression->is<IR::SelectExpression>()) {
            builder.endOfStatement();
            builder.newline();
        }
    }
    builder.blockEnd(false);
    return false;
}

bool ToNPL::preorder(const IR::P4Parser *c) {
    std::cout << "Enter ToNPL::preorder(const IR::P4Parser *c)" << std::endl;
    dump(1);
    bool decl = isDeclaration;
    isDeclaration = false;
    visit(c->type);
    isDeclaration = decl;
    if (c->constructorParams->size() != 0) visit(c->constructorParams);
    builder.spc();

    // No need to visit the states (TODO: deal with complex parsers)
    // builder.blockStart();
    // setVecSep("\n", "\n");
    // preorder(&c->parserLocals);
    // doneVec();
    // // explicit visit of parser states
    // for (auto s : c->states) {
    //     if (s->isBuiltin()) continue;
    //     builder.emitIndent();
    //     visit(s);
    //     builder.append("\n");
    // }
    // builder.blockEnd(true);
    return false;
}

bool ToNPL::preorder(const IR::ExpressionValue *v) {
    std::cout << "Enter ToNPL::preorder(const IR::ExpressionValue *v)" << v->toString() << std::endl;
    dump(2);
    visit(v->expression);
    builder.endOfStatement();
    std::cout << "Exit ToNPL::preorder(const IR::ExpressionValue *v)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::ActionListElement *ale) {
    std::cout << "Enter ToNPL::preorder(const IR::ActionListElement *ale)" << ale->toString() << std::endl;
    dump(3);
    if (!ale->annotations->annotations.empty()) {
        visit(ale->annotations);
        builder.spc();
    }
    visit(ale->expression);
    std::cout << "Exit ToNPL::preorder(const IR::ActionListElement *ale)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::ActionList *v) {
    std::cout << "Enter ToNPL::preorder(const IR::ActionList *v)" << v->toString() << std::endl;
    dump(2);
    builder.blockStart();
    setVecSep(";\n", ";\n");
    preorder(&v->actionList);
    doneVec();
    builder.blockEnd(false);
    std::cout << "Exit ToNPL::preorder(const IR::ActionList *v)" << std::endl;
    return false;
}

void print_kf(std::map<const IR::KeyElement *, cstring> kf) {
    for (auto &v : kf) {
        std::cout << "v.first->toString() = " << v.first->toString() << "v.second = " << v.second << std::endl;
    }
}

std::string update_type_of_expression(cstring str) {
    str = str.replace('<', '[');
    str = str.replace('>', ']');
    return str.c_str();
}

bool ToNPL::preorder(const IR::Key *v) {
    std::cout << "Enter ToNPL::preorder(const IR::Key *v)" << v->toString() << std::endl;
    dump(2);
    
    std::map<const IR::KeyElement *, cstring> kf;
    size_t len = 0;
    for (auto f : v->keyElements) {
        Util::SourceCodeBuilder builder;
        ToNPL rec(builder, showIR);

        f->expression->apply(rec);
        cstring s = builder.toString();
        if (s.size() > len) len = s.size();
        kf.emplace(f, s);
    }
    std::cout << "map size = " << kf.size() << std::endl;
    int num = 0;
    builder.append("keys ");
    builder.blockStart();
    if (kf.size() > 0) {
        for (auto f : v->keyElements) {
            // std::cout << "type is " << f->expression->type->toString() << std::endl; // showing the type of element
            cstring type_str = f->expression->type->toString();
            std::string updated_type_str = update_type_of_expression(type_str);
            std::string var = "V" + std::to_string(num);
            builder.emitIndent();
            builder.append(updated_type_str + "  " + var);
            builder.endOfStatement(true);
            num++;
        }
    }
    builder.blockEnd(true);
    // print_kf(kf); // New test what is stored in kf
    // Ori:
    // for (auto f : v->keyElements) {
    //     dump(2, f, 2);
    //     builder.emitIndent();
    //     cstring s = get(kf, f);
    //     builder.append(s);
    //     size_t spaces = len - s.size();
    //     builder.append(std::string(spaces, ' '));
    //     builder.append(": ");
    //     visit(f->matchType);
    //     if (!f->annotations->annotations.empty()) {
    //         builder.append(" ");
    //         visit(f->annotations);
    //     }
    //     builder.endOfStatement(true);
    // }
    builder.append("\tkey_construct() ");
    builder.blockStart();
    num = 0;
    for (auto f : v->keyElements) {
        dump(2, f, 2);
        builder.emitIndent();
        cstring s = get(kf, f);
        // Format: Vi = A.B.C.macda
        std::string var = "V" + std::to_string(num);
        num++;
        builder.append(var);
        builder.append(" = ");
        builder.append(s);
        builder.endOfStatement(true);
    }
    builder.blockEnd(false);
    std::cout << "Exit ToNPL::preorder(const IR::Key *v)" << std::endl;
    return false;
}

// Collect all actions from one table
std::vector<cstring> collect_action_from_list(const IR::ActionList *v) {
    std::vector<cstring> ret_vec;
    for (auto act : v->actionList) {
        ret_vec.push_back(act->toString());
    }
    return ret_vec;
}

bool ToNPL::preorder(const IR::Property *p) {
    std::cout << "Enter ToNPL::preorder(const IR::Property *p)" << p->toString() << std::endl;
    dump(1);
    if (!p->annotations->annotations.empty()) {
        visit(p->annotations);
        builder.spc();
    }
    if (p->isConstant) builder.append("const ");
    // ori: builder.append(p->name);
    // ori: builder.append(" = ");
    if (p->name == "key") {
        visit(p->value);
    } else if (p->name == "actions") {
        // ori: builder.append(p->name);
        // ori: builder.append(" = ");
        // ori: visit(p->value);

        // Collect all actions' names because we know that p->value is the type ()
        std::vector<cstring> vec = collect_action_from_list((const IR::ActionList*)p->value);
        // First of all, generate fields {} for all parameters in the action
        builder.append("fields ");
        builder.blockStart();
        for (cstring cstr : vec) {
            for (auto &a : action_para_map[cstr]) {
                builder.append("\t\t" + a.second + "   " + a.first + ";\n");
            }
        }
        builder.blockEnd(true);
        for (cstring cstr : vec) {
            builder.append(action_map[cstr]);
        }
    } else if (p->name == "size") {
        // DONE
        builder.append("maxsize : ");
        visit(p->value);
        builder.newline();
        // TODO: better way instead of setting \t
        builder.append("\tminsize : ");
        visit(p->value);
        // visit(p->value);
    } else if (p->name == "default_action") {
        // There is no default action in npl
        // ori: builder.append(p->name);
        // ori: builder.append(" = ");
        // ori: visit(p->value);
    }
    std::cout << "Exit ToNPL::preorder(const IR::Property *p)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::TableProperties *t) {
    add(count);
    std::cout << build_string(count);
    std::cout << "Enter ToNPL::preorder(const IR::TableProperties *t)" << std::endl;
    for (auto p : t->properties) {
        builder.emitIndent();
        visit(p);
        builder.newline();
    }
    std::cout << build_string(count);
    sub(count);
    std::cout << "Exit ToNPL::preorder(const IR::TableProperties *t)" << std::endl;
    return false;
}

bool ToNPL::preorder(const IR::EntriesList *l) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::EntriesList *l)" << std::endl;
    dump(1);
    builder.append("{");
    builder.newline();
    builder.increaseIndent();
    visit(&l->entries);
    builder.decreaseIndent();
    builder.emitIndent();
    builder.append("}");
    return false;
}

bool ToNPL::preorder(const IR::Entry *e) {
    std::cout << "这里暂时应该到不了 ToNPL::preorder(const IR::Entry *e)" << std::endl;
    dump(2);
    builder.emitIndent();
    if (e->keys->components.size() == 1)
        setListTerm("", "");
    else
        setListTerm("(", ")");
    visit(e->keys);
    doneList();
    builder.append(" : ");
    visit(e->action);
    if (!e->annotations->annotations.empty()) {
        visit(e->annotations);
    }
    builder.append(";");
    return false;
}

// Check the match type of this table
std::string find_match_type(const IR::P4Table *c) {
    const IR::Key* k =  c->getKey();
    if (!k) {
        return "exact";
    }
    IR::Vector<IR::KeyElement> keys = k->keyElements;
    for (const IR::KeyElement* key : keys) {
        if (key->matchType->toString() != "exact") {
            return key->matchType->toString().c_str();
        }
    }
    return "exact";
}

bool ToNPL::preorder(const IR::P4Table *c) {
    std::cout << "ToNPL::preorder(const IR::P4Table *c)" << std::endl;
    
    dump(2);
    if (!c->annotations->annotations.empty()) {
        visit(c->annotations);
        builder.spc();
    }
    // builder.append("table ");
    builder.append("logical_table ");
    builder.append(c->name);
    builder.spc();
    builder.blockStart();
    setVecSep("\n", "\n");
    // exact match -> table_type : index; other types of match -> table_type : tcam; 
    std::string tmp_str = find_match_type(c);
    assert(tmp_str == "exact" || tmp_str == "ternary" || tmp_str == "lpm");
    if (tmp_str == "exact") {
        builder.append("\ttable_type : index;\n");
    } else {
        builder.append("\ttable_type : tcam;\n");
    }
    visit(c->properties);
    doneVec();
    builder.blockEnd(false);
    return false;
}

// Need change (DONE) example output: standard_metadata_t
bool ToNPL::preorder(const IR::Path *p) {
    std::cout << "--------------ToNPL::preorder(const IR::Path *p)" << p->asString() << std::endl;
    // builder.append(p->asString());
    builder.append(p->asString());
    return false;
}

// No need to change
std::string toNPL(const IR::INode *node) {
    std::stringstream stream;
    P4::ToNPL toNPL(&stream, false);
    node->getNode()->apply(toNPL);
    return stream.str();
}
// No need to change
void dumpNPL(const IR::INode *node) {
    auto s = toNPL(node);
    std::cout << s;
}

}  // namespace P4
