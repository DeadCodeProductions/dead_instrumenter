#include "ValueRangeTagger.hpp"

#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <string>

using namespace clang;
using namespace ast_matchers;
using namespace transformer;

namespace protag {

detail::ValueRangeTaggerCallback::ValueRangeTaggerCallback(
    RewriteRule Rule,
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements,
    std::map<std::string, int> &FileToNumberValueRangeTrackers)
    : Rule{Rule}, FileToReplacements{FileToReplacements},
      FileToNumberValueRangeTrackers{FileToNumberValueRangeTrackers} {}

void detail::ValueRangeTaggerCallback::run(
    const clang::ast_matchers::MatchFinder::MatchResult &Result) {
    if (Result.Context->getDiagnostics().hasErrorOccurred()) {
        llvm::errs() << "An error has occured.\n";
        return;
    }
    Expected<SmallVector<transformer::Edit, 1>> Edits =
        transformer::detail::findSelectedCase(Result, Rule).Edits(Result);
    if (!Edits) {
        llvm::errs() << "Rewrite failed: " << llvm::toString(Edits.takeError())
                     << "\n";
        return;
    }
    auto SM = Result.SourceManager;
    for (const auto &T : *Edits) {
        assert(T.Kind == transformer::EditKind::Range);
        auto FilePath = dead::GetFilenameFromRange(T.Range, *SM);
        auto N = FileToNumberValueRangeTrackers[FilePath]++;
        auto R = tooling::Replacement(
            *SM, T.Range, "(ValueRangeTag" + std::to_string(N) + T.Replacement);
        auto &Replacements = FileToReplacements[FilePath];
        auto Err = Replacements.add(R);
        if (Err) {
            auto NewOffset = Replacements.getShiftedCodePosition(R.getOffset());
            auto NewLength = Replacements.getShiftedCodePosition(
                                 R.getOffset() + R.getLength()) -
                             NewOffset;
            if (NewLength == R.getLength()) {
                R = clang::tooling::Replacement(R.getFilePath(), NewOffset,
                                                NewLength,
                                                R.getReplacementText());
                Replacements = Replacements.merge(tooling::Replacements(R));
            } else {
                llvm_unreachable(llvm::toString(std::move(Err)).c_str());
            }
        }
    }
}

void detail::ValueRangeTaggerCallback::registerMatchers(
    clang::ast_matchers::MatchFinder &Finder) {
    for (auto &Matcher : transformer::detail::buildMatchers(Rule))
        Finder.addDynamicMatcher(
            Matcher.withTraversalKind(clang::TK_IgnoreUnlessSpelledInSource),
            this);
}

namespace {

auto handleLocalVars() {
    auto matcher =
        expr(declRefExpr(to(varDecl(hasLocalStorage()))).bind("varref"),
             unless(hasAncestor(binaryOperator(
                 isAssignmentOperator(), hasLHS(equalsBoundNode("varref"))))),
             unless(hasParent(
                 unaryOperator(hasAnyOperatorName("++", "--", "&"),
                               hasUnaryOperand(equalsBoundNode("varref"))))));
    return makeRule(
        matcher, changeTo(node("varref"),
                          cat("(", node("varref"), "),", node("varref"), ")")));
}

} // namespace

ValueRangeTagger::ValueRangeTagger(
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements)
    : Callbacks{detail::ValueRangeTaggerCallback{
          handleLocalVars(), FileToReplacements,
          FileToNumberValueRangeTrackers}} {}

void ValueRangeTagger::registerMatchers(
    clang::ast_matchers::MatchFinder &Finder) {
    for (auto &Callback : Callbacks)
        Callback.registerMatchers(Finder);
}

} // namespace protag
