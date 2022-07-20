#pragma once

#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Tooling/Transformer/RewriteRule.h>

namespace dead {

std::string GetFilenameFromRange(const clang::CharSourceRange &R,
                                 const clang::SourceManager &SM);

namespace detail {
class RuleActionCallback
    : public clang::ast_matchers::MatchFinder::MatchCallback {
  public:
    RuleActionCallback(
        clang::transformer::RewriteRule Rule,
        std::map<std::string, clang::tooling::Replacements> &FileToReplacements)
        : Rule{Rule}, FileToReplacements{FileToReplacements} {}
    void
    run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;
    void registerMatchers(clang::ast_matchers::MatchFinder &Finder);

  private:
    clang::transformer::RewriteRule Rule;
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements;
};

class RuleActionEditCollector
    : public clang::ast_matchers::MatchFinder::MatchCallback {
  public:
    RuleActionEditCollector(
        clang::transformer::RewriteRule Rule,
        std::vector<clang::tooling::Replacement> &Replacements,
        std::map<std::string, int> &FileToNumberMarkerDecls)
        : Rule{Rule}, Replacements{Replacements},
          FileToNumberMarkerDecls{FileToNumberMarkerDecls} {}
    void
    run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;
    void registerMatchers(clang::ast_matchers::MatchFinder &Finder);

  private:
    clang::transformer::RewriteRule Rule;
    std::vector<clang::tooling::Replacement> &Replacements;
    std::map<std::string, int> &FileToNumberMarkerDecls;
};

} // namespace detail
} // namespace dead

