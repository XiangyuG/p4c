#include "backends/p4tools/common/lib/variables.h"

#include <map>
#include <string>
#include <tuple>

#include "ir/id.h"

namespace P4Tools {

const IR::PathExpression ToolsVariables::VAR_PREFIX = IR::PathExpression("p4tools*var");

const IR::StateVariable &ToolsVariables::getStateVariable(const IR::Type *type, cstring name) {
    // TODO: Is caching worth it here?
    // TODO: Do we really need to "allocate" a state variable? They are immediately consumed  by the
    // symbolic environment.
    return *new IR::StateVariable(new IR::Member(type, &VAR_PREFIX, name));
}

const IR::SymbolicVariable *ToolsVariables::getSymbolicVariable(const IR::Type *type,
                                                                int incarnation, cstring name) {
    // TODO: Is caching worth it here?
    auto symbolicName = name + "_" + std::to_string(incarnation);
    return new IR::SymbolicVariable(type, symbolicName);
}

const cstring ToolsVariables::VALID = "*valid";

IR::StateVariable ToolsVariables::getHeaderValidity(const IR::Expression *headerRef) {
    return new IR::Member(IR::Type::Boolean::get(), headerRef, VALID);
}

const IR::TaintExpression *ToolsVariables::getTaintExpression(const IR::Type *type) {
    // Do not cache varbits.
    if (type->is<IR::Extracted_Varbits>()) {
        return new IR::TaintExpression(type);
    }
    // Only cache bits with width lower than 16 bit to restrict the size of the cache.
    const auto *tb = type->to<IR::Type_Bits>();
    if (type->width_bits() > 16 || tb == nullptr) {
        return new IR::TaintExpression(type);
    }
    // Taint expressions are interned. Keys in the intern map is the signedness and width of the
    // type.
    using key_t = std::tuple<int, bool>;
    static std::map<key_t, const IR::TaintExpression *> TAINTS;

    auto *&result = TAINTS[{tb->width_bits(), tb->isSigned}];
    if (result == nullptr) {
        result = new IR::TaintExpression(type);
    }

    return result;
}

}  // namespace P4Tools
