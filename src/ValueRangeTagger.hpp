#pragma once

#include "Common.hpp"

#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Transformer/RewriteRule.h>

namespace protag {

namespace detail {
class ValueRangeTaggerCallback
    : public clang::ast_matchers::MatchFinder::MatchCallback {
  public:
    ValueRangeTaggerCallback(
        clang::transformer::RewriteRule Rule,
        std::map<std::string, clang::tooling::Replacements> &FileToReplacements,
        std::map<std::string, int> &FileToNumberValueRangeTrackers);
    void
    run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;
    void registerMatchers(clang::ast_matchers::MatchFinder &Finder);

  private:
    clang::transformer::RewriteRule Rule;
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements;
    std::map<std::string, int> &FileToNumberValueRangeTrackers;
};

} // namespace detail

class ValueRangeTagger {
  public:
    ValueRangeTagger(std::map<std::string, clang::tooling::Replacements>
                         &FileToReplacements);
    ValueRangeTagger(const ValueRangeTagger &) = delete;
    ValueRangeTagger(ValueRangeTagger &&) = delete;

    void registerMatchers(clang::ast_matchers::MatchFinder &Finder);

  private:
    std::vector<detail::ValueRangeTaggerCallback> Callbacks;
    std::map<std::string, int> FileToNumberValueRangeTrackers;
};
} // namespace protag
