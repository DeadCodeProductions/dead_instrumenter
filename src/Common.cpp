#include "Common.hpp"
#include <clang/AST/ASTTypeTraits.h>

using namespace llvm;
using namespace clang;
using namespace tooling;
using namespace transformer;

namespace dead {

std::string GetFilenameFromRange(const CharSourceRange &R,
                                 const SourceManager &SM) {
    const std::pair<FileID, unsigned> DecomposedLocation =
        SM.getDecomposedLoc(SM.getSpellingLoc(R.getBegin()));
    const FileEntry *Entry = SM.getFileEntryForID(DecomposedLocation.first);
    return std::string(Entry ? Entry->getName() : "");
}



void detail::RuleActionCallback::registerMatchers(
    ast_matchers::MatchFinder &Finder) {
    for (auto &Matcher : transformer::detail::buildMatchers(Rule))
        Finder.addDynamicMatcher(
            Matcher.withTraversalKind(clang::TK_IgnoreUnlessSpelledInSource),
            this);
}

void detail::RuleActionCallback::run(
    const ast_matchers::MatchFinder::MatchResult &Result) {
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
        auto R = tooling::Replacement(*SM, T.Range, T.Replacement);
        auto &Replacements = FileToReplacements[std::string(R.getFilePath())];
        auto Err = Replacements.add(R);
        if (Err) {
            auto NewOffset = Replacements.getShiftedCodePosition(R.getOffset());
            auto NewLength = Replacements.getShiftedCodePosition(
                                 R.getOffset() + R.getLength()) -
                             NewOffset;
            if (NewLength == R.getLength()) {
                R = Replacement(R.getFilePath(), NewOffset, NewLength,
                                R.getReplacementText());
                Replacements = Replacements.merge(tooling::Replacements(R));
            } else {
                llvm_unreachable(llvm::toString(std::move(Err)).c_str());
            }
        }
    }
}

} // namespace dead
