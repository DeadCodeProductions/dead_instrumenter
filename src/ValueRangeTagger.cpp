#include "ValueRangeTagger.hpp"

#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/Stencil.h>

using namespace clang;
using namespace ast_matchers;
using namespace transformer;

namespace protag {

namespace {

auto handleLocalVars() {
    auto matcher =
        // declRefExpr(to(varDecl(hasLocalStorage())),
        // hasParent(stmt(hasParent(compoundStmt())).bind("stmt")))
        //.bind("varref");

        declRefExpr(to(varDecl(hasLocalStorage())),
                    hasParent(stmt().bind("stmt")))
            .bind("varref");

    return makeRule(matcher,
                    changeTo(node("varref"), cat("(bar(", node("varref"), "),",
                                                 node("varref"), ")")));
}

} // namespace

ValueRangeTagger::ValueRangeTagger(
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements)
    : FileToReplacements{FileToReplacements},
      Rules{dead::detail::RuleActionCallback{handleLocalVars(),
                                             FileToReplacements}} {}

void ValueRangeTagger::registerMatchers(
    clang::ast_matchers::MatchFinder &Finder) {
    for (auto &Rule : Rules)
        Rule.registerMatchers(Finder);
}

} // namespace protag
