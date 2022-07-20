#pragma once

#include "Common.hpp"

#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Tooling/Core/Replacement.h>

namespace protag {

class ValueRangeTagger {
  public:
    ValueRangeTagger(std::map<std::string, clang::tooling::Replacements>
                         &FileToReplacements);
    ValueRangeTagger(const ValueRangeTagger &) = delete;
    ValueRangeTagger(ValueRangeTagger &&) = delete;

    void registerMatchers(clang::ast_matchers::MatchFinder &Finder);

  private:
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements;
    std::vector<dead::detail::RuleActionCallback> Rules;
};
} // namespace protag
