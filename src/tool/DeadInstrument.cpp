#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Refactoring.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <type_traits>

#include <DeadInstrumenter.hpp>
#include <ValueRangeTagger.hpp>

using namespace llvm;
using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;

namespace {

enum class ToolMode { MakeGlobalsStaticOnly, InstrumentBranches, TagVariables };

cl::opt<ToolMode>
    Mode("mode", cl::desc("dead-instrumenter mode:"),
         cl::values(
             clEnumValN(ToolMode::MakeGlobalsStaticOnly, "globals",
                        "Only make globals static"),
             clEnumValN(ToolMode::TagVariables, "tag-variables",
                        "Tag variables for value range analysis testing"),
             clEnumValN(ToolMode::InstrumentBranches, "instrument",
                        "Only canonicalize and instrument branches (default)")),
         cl::init(ToolMode::InstrumentBranches),
         cl::cat(dead::DeadInstrOptions));

template <typename InstrTool> int runToolOnCode(RefactoringTool &Tool) {
    InstrTool Instr(Tool.getReplacements());
    ast_matchers::MatchFinder Finder;
    Instr.registerMatchers(Finder);
    std::unique_ptr<tooling::FrontendActionFactory> Factory =
        tooling::newFrontendActionFactory(&Finder);

    auto Ret = Tool.run(Factory.get());
    if constexpr (std::is_same_v<InstrTool, dead::Instrumenter>)
        if (!Ret)
            Instr.applyReplacements();
    return Ret;
}

bool applyReplacements(RefactoringTool &Tool) {
    LangOptions DefaultLangOptions;
    IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
    clang::TextDiagnosticPrinter DiagnosticPrinter(errs(), &*DiagOpts);
    DiagnosticsEngine Diagnostics(
        IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs()), &*DiagOpts,
        &DiagnosticPrinter, false);
    auto &FileMgr = Tool.getFiles();
    SourceManager Sources(Diagnostics, FileMgr);

    Rewriter Rewrite(Sources, DefaultLangOptions);

    bool Result = true;
    for (const auto &FileAndReplaces : groupReplacementsByFile(
             Rewrite.getSourceMgr().getFileManager(), Tool.getReplacements())) {
        auto &CurReplaces = FileAndReplaces.second;

        Result = applyAllReplacements(CurReplaces, Rewrite) && Result;
    }
    if (!Result) {
        llvm::errs() << "Failed applying all replacements.\n";
        return false;
    }

    return !Rewrite.overwriteChangedFiles();
}

void versionPrinter(llvm::raw_ostream &S) { S << "value_range_tags2\n"; }

} // namespace

int main(int argc, const char **argv) {
    cl::SetVersionPrinter(versionPrinter);
    auto ExpectedParser =
        CommonOptionsParser::create(argc, argv, dead::DeadInstrOptions);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    const auto &Compilations = OptionsParser.getCompilations();
    const auto &Files = OptionsParser.getSourcePathList();
    RefactoringTool Tool(Compilations, Files);
    int Result = 0;
    switch (Mode) {
    case ToolMode::MakeGlobalsStaticOnly:
        Result = runToolOnCode<dead::GlobalStaticMaker>(Tool);
        break;
    case ToolMode::InstrumentBranches:
        Result = runToolOnCode<dead::Instrumenter>(Tool);
        break;
    case ToolMode::TagVariables:
        Result = runToolOnCode<protag::ValueRangeTagger>(Tool);
        break;
    }
    if (Result) {
        llvm::errs() << "Something went wrong...\n";
        return Result;
    }
    if (!applyReplacements(Tool)) {
        llvm::errs() << "Failed to overwrite the input files.\n";
        return 1;
    }

    return 0;
}
