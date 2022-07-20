#pragma once

#include "Common.hpp"

#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <llvm/Support/CommandLine.h>

namespace dead {

extern llvm::cl::OptionCategory DeadInstrOptions;

namespace detail {
void setEmitDisableMacros(bool);
} // namespace detail

// Makes global variables static
class GlobalStaticMaker {
  public:
    GlobalStaticMaker(std::map<std::string, clang::tooling::Replacements>
                          &FileToReplacements);
    void registerMatchers(clang::ast_matchers::MatchFinder &Finder);

  private:
    detail::RuleActionCallback Rule;
};

// Adds DCEMarkers in places where control flow diverges
class Instrumenter {
  public:
    Instrumenter(std::map<std::string, clang::tooling::Replacements>
                     &FileToReplacements);
    Instrumenter(Instrumenter &&) = delete;
    Instrumenter(const Instrumenter &) = delete;

    void registerMatchers(clang::ast_matchers::MatchFinder &Finder);
    void applyReplacements();

  private:
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements;
    std::vector<detail::RuleActionEditCollector> Rules;
    std::vector<clang::tooling::Replacement> Replacements;
    std::map<std::string, int> FileToNumberMarkerDecls;
};
} // namespace dead
