#include "DeadInstrumenter.hpp"

#include "clang/Tooling/Transformer/SourceCode.h"
#include <clang/AST/Decl.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchersMacros.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Lex/Lexer.h>
#include <clang/Sema/Ownership.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Transformer/MatchConsumer.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <cstddef>
#include <limits>
#include <llvm/ADT/Any.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace clang::transformer;
using namespace clang::transformer::detail;
namespace cl = llvm::cl;

namespace dead {

llvm::cl::OptionCategory DeadInstrOptions("dead-instrument options");

namespace {

cl::opt<bool>
    EmitDisableMacros("emit-disable-macros",
                      cl::desc("Emit ifdefs for disabling code related to "
                               "markers that have been found to be dead."),
                      cl::init(false), cl::cat(DeadInstrOptions));

enum class EditMetadataKind {
    MarkerDecl,
    MarkerCall,
    MacroDisableBlockBegin,
    MacroDisableBlockBeginPre,
    IfPrologue,
    IfAtLeastOneDefined,
    IfAtLeastOneDefinedElse,
    NewElseBranch
};
std::string GetFilenameFromRange(const CharSourceRange &R,
                                 const SourceManager &SM) {
    const std::pair<FileID, unsigned> DecomposedLocation =
        SM.getDecomposedLoc(SM.getSpellingLoc(R.getBegin()));
    const FileEntry *Entry = SM.getFileEntryForID(DecomposedLocation.first);
    return std::string(Entry ? Entry->getName() : "");
}

auto TwoDeleteMacrosTemplate(int N1, int N2, StringRef Op) {
    return ("\n\n#if !defined(DeleteBlockDCEMarker" + std::to_string(N1) +
            "_) " + Op + " !defined(DeleteBlockDCEMarker" + std::to_string(N2) +
            "_)\n\n")
        .str();
}

auto GetBothDefinedText(int N1, int N2) {
    return TwoDeleteMacrosTemplate(N1, N2, "||");
}

auto GetAtLeastOneDefinedText(int N1, int N2) {
    return TwoDeleteMacrosTemplate(N1, N2, "&&");
}

auto GetDeleteMacroIfDefText(int N) {
    return "\n\n#ifndef DeleteBlockDCEMarker" + std::to_string(N) + "_\n\n";
}

} // namespace

void detail::setEmitDisableMacros(bool val) { EmitDisableMacros = val; }

void detail::RuleActionEditCollector::run(
    const clang::ast_matchers::MatchFinder::MatchResult &Result) {
    if (Result.Context->getDiagnostics().hasErrorOccurred()) {
        llvm::errs() << "An error has occured.\n";
        return;
    }
    Expected<SmallVector<transformer::Edit, 1>> Edits =
        findSelectedCase(Result, Rule).Edits(Result);
    if (!Edits) {
        llvm::errs() << "Rewrite failed: " << llvm::toString(Edits.takeError())
                     << "\n";
        return;
    }
    auto SM = Result.SourceManager;
    for (const auto &T : *Edits) {
        assert(T.Kind == transformer::EditKind::Range);
        const auto *Metadata =
            T.Metadata.hasValue()
                ? llvm::any_cast<EditMetadataKind>(&T.Metadata)
                : nullptr;

        auto UpdateReplacements = [&](llvm::StringRef Text) {
            Replacements.emplace_back(*SM, T.Range, Text);
        };

        if (!Metadata) {
            UpdateReplacements(T.Replacement);
            continue;
        }

        auto GetMarkerN = [&]() -> int & {
            return FileToNumberMarkerDecls[GetFilenameFromRange(T.Range, *SM)];
        };

        switch (*Metadata) {
        case EditMetadataKind::MarkerCall: {
            auto N = GetMarkerN()++;
            UpdateReplacements(T.Replacement + "\n\nDCEMarker" +
                               std::to_string(N) + "_();\n\n");
            break;
        }
        case EditMetadataKind::MacroDisableBlockBegin: {
            auto N = GetMarkerN() - 1;
            UpdateReplacements(T.Replacement + GetDeleteMacroIfDefText(N));
            break;
        }
        case EditMetadataKind::MacroDisableBlockBeginPre: {
            auto N = GetMarkerN() - 1;
            UpdateReplacements(GetDeleteMacroIfDefText(N) + T.Replacement);
            break;
        }
        case EditMetadataKind::IfPrologue: {
            auto N = GetMarkerN();
            UpdateReplacements(GetBothDefinedText(N, N + 1) +
                               GetAtLeastOneDefinedText(N, N + 1) +
                               T.Replacement);
            break;
        }
        case EditMetadataKind::IfAtLeastOneDefined: {
            auto N = GetMarkerN();
            UpdateReplacements(T.Replacement +
                               GetAtLeastOneDefinedText(N, N + 1));
            break;
        }
        case EditMetadataKind::IfAtLeastOneDefinedElse: {
            auto N = GetMarkerN();
            UpdateReplacements(T.Replacement +
                               GetAtLeastOneDefinedText(N - 2, N - 1));
            break;
        }
        case EditMetadataKind::NewElseBranch: {
            auto N = GetMarkerN()++;
            UpdateReplacements(
                T.Replacement +
                (EmitDisableMacros ? GetAtLeastOneDefinedText(N, N + 1)
                                   : std::string{"\n\n"}) +
                " else {\nDCEMarker" + std::to_string(N) + "_();\n}" +
                (EmitDisableMacros ? "\n#endif" : "") + "\n\n");
            break;
        }
        default:
            llvm_unreachable("dead::detail::RuleActionEditCollector::run: "
                             "Unknown EditMetadataKind");
        };
    }
}

void detail::RuleActionEditCollector::registerMatchers(
    clang::ast_matchers::MatchFinder &Finder) {
    for (auto &Matcher : buildMatchers(Rule))
        Finder.addDynamicMatcher(Matcher, this);
}

namespace {

AST_MATCHER(VarDecl, isExtern) {
    (void)Finder;
    (void)Builder;
    return Node.hasExternalStorage();
}

AST_MATCHER(Stmt, notInMacro) {
    (void)Finder;
    (void)Builder;
    return !Node.getBeginLoc().isMacroID() && !Node.getEndLoc().isMacroID();
}

Expected<DynTypedNode> getNode(const ast_matchers::BoundNodes &Nodes,
                               StringRef ID) {
    auto &NodesMap = Nodes.getMap();
    auto It = NodesMap.find(ID);
    if (It == NodesMap.end())
        return llvm::make_error<llvm::StringError>(llvm::errc::invalid_argument,
                                                   ID + "not bound");
    return It->second;
}

ASTEdit addMetadata(ASTEdit &&Edit, EditMetadataKind Kind) {
    return withMetadata(
        std::move(Edit),
        [Kind](const clang::ast_matchers::MatchFinder::MatchResult &)
            -> EditMetadataKind { return Kind; });
}

ASTEdit addMarkerAfter(RangeSelector &&Selection, Stencil Text) {
    return addMetadata(insertAfter(std::move(Selection), std::move(Text)),
                       EditMetadataKind::MarkerCall);
}

ASTEdit addMarkerBefore(RangeSelector &&Selection, Stencil Text) {
    return addMetadata(insertBefore(std::move(Selection), std::move(Text)),
                       EditMetadataKind::MarkerCall);
}

ASTEdit addDeleteMacro(RangeSelector &&Selection, Stencil Text) {
    return addMetadata(insertBefore(std::move(Selection), std::move(Text)),
                       EditMetadataKind::MacroDisableBlockBegin);
}

ASTEdit addDeleteMacroPre(ASTEdit Edit) {
    return addMetadata(std::move(Edit),
                       EditMetadataKind::MacroDisableBlockBeginPre);
}

ASTEdit addElseBranch(RangeSelector &&Selection, Stencil Text) {
    return addMetadata(insertAfter(std::move(Selection), std::move(Text)),
                       EditMetadataKind::NewElseBranch);
}

ASTEdit addIfAtLeastOneDefined(ASTEdit Edit) {
    return addMetadata(std::move(Edit), EditMetadataKind::IfAtLeastOneDefined);
}

ASTEdit addIfAtLeastOneDefinedElse(ASTEdit Edit) {
    return addMetadata(std::move(Edit),
                       EditMetadataKind::IfAtLeastOneDefinedElse);
}

ASTEdit addIfPrologue(ASTEdit Edit) {
    return addMetadata(std::move(Edit), EditMetadataKind::IfPrologue);
}

template <typename T>
SourceLocation handleReturnStmts(const T &Node, SourceLocation End,
                                 const SourceManager &SM) {
    if (const auto &Ret = Node.template get<ReturnStmt>()) {
        if (Ret->getRetValue())
            return End;
        // ReturnStmts without an Expr have a broken range...
        return End.getLocWithOffset(7);
    }
    // The end loc might be pointing to a nested return...
    bool Invalid;
    const char *Char = SM.getCharacterData(End, &Invalid);
    if (Invalid)
        return End;
    auto Return = "return";
    for (int i = 0; i < 5; ++i)
        if (Char[i] != Return[i])
            return End;
    return End.getLocWithOffset(7);
}

template <typename T>
CharSourceRange getExtendedRangeWithCommentsAndSemi(const T &Node,
                                                    ASTContext &Context) {
    auto &SM = Context.getSourceManager();
    auto Range = CharSourceRange::getTokenRange(Node.getSourceRange());
    Range = SM.getExpansionRange(Range);
    Range.setEnd(handleReturnStmts(Node, Range.getEnd(), SM));
    Range = maybeExtendRange(Range, tok::TokenKind::comment, Context);
    return maybeExtendRange(Range, tok::TokenKind::semi, Context);
}

RangeSelector statementWithMacrosExpanded(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        auto Range = SM.getExpansionRange(
            getExtendedRangeWithCommentsAndSemi(*Node, *Result.Context));
        return Range;
    };
}

auto inMainAndNotMacro = allOf(notInMacro(), isExpansionInMainFile());

RangeSelector CStmtLBrace(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(Node->get<CompoundStmt>()->getLBracLoc());
    };
}

RangeSelector CStmtRBrace(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(Node->get<CompoundStmt>()->getRBracLoc());
    };
}

auto InstrumentCStmt(std::string id, bool with_macro = false) {
    if (with_macro)
        return flatten(insertAfter(CStmtRBrace(id), cat("\n\n#endif\n\n")),
                       addMarkerBefore(statements(id), cat("")),
                       addDeleteMacro(CStmtLBrace(id), cat("\n\n")));
    return edit(addMarkerBefore(statements(id), cat("")));
}

EditGenerator InstrumentNonCStmt(std::string id,
                                 bool with_delete_macro = false) {
    return flatten(
        addMarkerBefore(statementWithMacrosExpanded(id), cat("\n\n{\n\n")),
        with_delete_macro
            ? edit(addDeleteMacro(statementWithMacrosExpanded(id), cat("\n\n")))
            : noEdits(),
        insertAfter(statementWithMacrosExpanded(id),
                    with_delete_macro ? cat("\n\n}\n\n#endif\n\n")
                                      : cat("\n\n}\n\n")

                        ));
}

AST_MATCHER(IfStmt, ConditionNotInMacroAndInMain) {
    (void)Builder;
    auto RParenLoc = Node.getRParenLoc();
    auto LParenLoc = Node.getLParenLoc();
    auto IfLoc = Node.getIfLoc();
    const auto &SM = Finder->getASTContext().getSourceManager();

    if (RParenLoc.isMacroID() ||
        !SM.isInMainFile(SM.getExpansionLoc(RParenLoc)))
        return false;

    if (LParenLoc.isMacroID() ||
        !SM.isInMainFile(SM.getExpansionLoc(LParenLoc)))
        return false;
    return !IfLoc.isMacroID() && SM.isInMainFile(SM.getExpansionLoc(IfLoc));
}

AST_MATCHER(IfStmt, ElseNotInMacroAndInMain) {
    (void)Builder;
    auto ElseLoc = Node.getElseLoc();
    const auto &SM = Finder->getASTContext().getSourceManager();
    return !ElseLoc.isMacroID() && SM.isInMainFile(SM.getExpansionLoc(ElseLoc));
}

RangeSelector LParenLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        if (const auto *IfStmt_ = Node->get<IfStmt>()) {
            return SM.getExpansionRange(IfStmt_->getLParenLoc());
        }
        if (const auto *ForLoop_ = Node->get<ForStmt>()) {
            return SM.getExpansionRange(ForLoop_->getLParenLoc());
        }
        if (const auto *WhileLoop_ = Node->get<WhileStmt>()) {
            return SM.getExpansionRange(WhileLoop_->getLParenLoc());
        }
        llvm_unreachable("LParenLoc::invalid node kind");
    };
}

RangeSelector RParenLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        if (const auto *IfStmt_ = Node->get<IfStmt>()) {
            return SM.getExpansionRange(IfStmt_->getRParenLoc());
        }
        if (const auto *ForLoop_ = Node->get<ForStmt>()) {
            return SM.getExpansionRange(ForLoop_->getRParenLoc());
        }
        if (const auto *WhileLoop_ = Node->get<WhileStmt>()) {
            return SM.getExpansionRange(WhileLoop_->getRParenLoc());
        }
        llvm_unreachable("LParenLoc::invalid node kind");
    };
}

RangeSelector IfLParenLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(Node->get<IfStmt>()->getLParenLoc());
    };
}

RangeSelector IfRParenLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(Node->get<IfStmt>()->getRParenLoc());
    };
}

RangeSelector ElseLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(Node->get<IfStmt>()->getElseLoc());
    };
}

RangeSelector IfLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(Node->get<IfStmt>()->getIfLoc());
    };
}

auto handleIfStmt() {
    auto matcher =
        ifStmt(ConditionNotInMacroAndInMain(),
               optionally(hasElse(
                   anyOf(compoundStmt(inMainAndNotMacro).bind("celse"),
                         stmt(hasParent(ifStmt(ElseNotInMacroAndInMain())))
                             .bind("else")))),
               hasThen(anyOf(compoundStmt(inMainAndNotMacro).bind("cthen"),
                             stmt().bind("then"))))
            .bind("ifstmt");
    auto actions = flatten(
        // ifdef magic
        EmitDisableMacros
            ? flatten(insertAfter(statementWithMacrosExpanded("ifstmt"),
                                  cat("\n\n#endif\n\n")),
                      insertAfter(IfLParenLoc("ifstmt"), cat("\n\n#endif\n\n")),
                      addIfAtLeastOneDefined(
                          insertBefore(IfRParenLoc("ifstmt"), cat(""))),
                      addIfPrologue(changeTo(IfLoc("ifstmt"), cat("if"))))
            : noEdits(),
        // instrument else branch
        ifBound("celse", InstrumentCStmt("celse", EmitDisableMacros),
                ifBound("else", InstrumentNonCStmt("else", EmitDisableMacros),
                        edit(addElseBranch(
                            statementWithMacrosExpanded("ifstmt"), cat(""))))),
        // instrument then branch
        ifBound("cthen", InstrumentCStmt("cthen", EmitDisableMacros),
                ifBound("then", InstrumentNonCStmt("then", EmitDisableMacros),
                        noEdits())),
        EmitDisableMacros ? edit(insertAfter(IfRParenLoc("ifstmt"),
                                             cat("\n\n#else\n\n;\n#endif\n\n")))
                          : noEdits(),
        EmitDisableMacros
            ? ifBound("celse",
                      flatten(addIfAtLeastOneDefinedElse(
                                  insertBefore(ElseLoc("ifstmt"), cat(""))),
                              insertBefore(statementWithMacrosExpanded("celse"),
                                           cat("\n\n#endif\n\n"))),
                      ifBound("else",
                              flatten(addIfAtLeastOneDefinedElse(insertBefore(
                                          ElseLoc("ifstmt"), cat(""))),
                                      insertBefore(
                                          statementWithMacrosExpanded("else"),
                                          cat("\n\n#endif\n\n"))),
                              noEdits()))
            : noEdits());
    return makeRule(matcher, actions);
}

RangeSelector doStmtWhile(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(
            SourceRange(Node->get<DoStmt>()->getWhileLoc()));
    };
}

AST_MATCHER(DoStmt, DoAndWhileNotMacroAndInMain) {
    (void)Builder;
    const auto &SM = Finder->getASTContext().getSourceManager();
    const auto &DoLoc = Node.getDoLoc();
    const auto &WhileLoc = Node.getWhileLoc();
    return !Node.getDoLoc().isMacroID() && !Node.getWhileLoc().isMacroID() &&
           SM.isInMainFile(SM.getExpansionLoc(DoLoc)) &&
           SM.isInMainFile(SM.getExpansionLoc(WhileLoc));
}

RangeSelector doBegin(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(
            SourceRange(Node->get<DoStmt>()->getDoLoc()));
    };
}

auto handleDoWhile() {
    auto compoundMatcher =
        doStmt(inMainAndNotMacro,
               hasBody(compoundStmt(inMainAndNotMacro).bind("body")))
            .bind("dostmt");
    auto nonCompoundLoopMatcher =
        doStmt(DoAndWhileNotMacroAndInMain(), hasBody(stmt().bind("body")))
            .bind("dostmt");

    auto macroActions =
        flatten(addDeleteMacroPre(changeTo(doBegin("dostmt"), cat("do"))),
                insertAfter(statementWithMacrosExpanded("dostmt"),
                            cat("\n\n#endif\n\n"))

        );

    auto doWhileAction = flatten(
        addMarkerBefore(statementWithMacrosExpanded("body"), cat("")),
        insertBefore(statementWithMacrosExpanded("body"), cat("\n\n{\n\n")),
        insertBefore(doStmtWhile("dostmt"), cat("\n\n}\n\n")));

    return applyFirst(
        {makeRule(compoundMatcher,
                  flatten(InstrumentCStmt("body", false),
                          EmitDisableMacros ? macroActions : noEdits())),
         makeRule(nonCompoundLoopMatcher,
                  flatten(doWhileAction,
                          EmitDisableMacros ? macroActions : noEdits()))});
}

RangeSelector forBegin(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(
            SourceRange(Node->get<ForStmt>()->getForLoc()));
    };
}

auto SecondSemiForLoop(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        if (const Expr *Inc = Node->get<ForStmt>()->getInc())
            return SM.getExpansionRange(SourceRange(Inc->getBeginLoc()));

        return SM.getExpansionRange(
            SourceRange(Node->get<ForStmt>()->getRParenLoc()));
    };
}

auto handleFor() {
    auto compoundMatcher =
        forStmt(inMainAndNotMacro,
                hasBody(compoundStmt(inMainAndNotMacro).bind("body")))
            .bind("loop");
    auto nonCompoundLoopMatcher =
        forStmt(inMainAndNotMacro,
                hasBody(stmt(inMainAndNotMacro).bind("body")))
            .bind("loop");
    auto macroActions =
        flatten(addDeleteMacroPre(changeTo(forBegin("loop"), cat("for"))),
                insertAfter(LParenLoc("loop"), cat("\n\n#else\n{\n#endif\n\n")),
                addDeleteMacro(SecondSemiForLoop("loop"), cat("\n\n")),
                insertAfter(RParenLoc("loop"), cat("\n\n#endif\n\n")));
    return applyFirst(
        {makeRule(
             compoundMatcher,
             flatten(
                 EmitDisableMacros ? edit(insertBefore(CStmtRBrace("body"),
                                                       cat("\n\n#endif\n\n")))
                                   : noEdits(),
                 addMarkerBefore(statements("body"), cat("")),
                 EmitDisableMacros
                     ? edit(addDeleteMacro(CStmtLBrace("body"), cat("\n\n")))
                     : noEdits(),
                 EmitDisableMacros ? macroActions : noEdits())),
         makeRule(
             nonCompoundLoopMatcher,
             flatten(addMarkerBefore(statementWithMacrosExpanded("body"),
                                     cat("\n\n{\n\n")),
                     EmitDisableMacros
                         ? edit(addDeleteMacro(
                               statementWithMacrosExpanded("body"), cat("")))
                         : noEdits(),
                     insertAfter(statementWithMacrosExpanded("body"),
                                 EmitDisableMacros ? cat("\n\n#endif\n}\n\n")
                                                   : cat("\n\n}\n\n")),
                     EmitDisableMacros ? macroActions : noEdits()))});
}

RangeSelector whileBegin(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(
            SourceRange(Node->get<WhileStmt>()->getWhileLoc()));
    };
}

auto handleWhile() {
    auto compoundMatcher =
        whileStmt(inMainAndNotMacro,
                  hasBody(compoundStmt(inMainAndNotMacro).bind("body")))
            .bind("loop");
    auto nonCompoundLoopMatcher =
        whileStmt(inMainAndNotMacro,
                  hasBody(stmt(inMainAndNotMacro).bind("body")))
            .bind("loop");
    auto macroActions = flatten(
        addDeleteMacroPre(changeTo(whileBegin("loop"), cat("while"))),
        insertAfter(LParenLoc("loop"), cat("\n\n#endif\n\n")),
        addDeleteMacro(RParenLoc("loop"), cat("\n\n")),
        insertAfter(RParenLoc("loop"), cat("\n\n#else\n;\n#endif\n\n")));
    return applyFirst(
        {makeRule(compoundMatcher,
                  flatten(InstrumentCStmt("body", EmitDisableMacros),
                          EmitDisableMacros ? macroActions : noEdits())),
         makeRule(nonCompoundLoopMatcher,
                  flatten(InstrumentNonCStmt("body", EmitDisableMacros),
                          EmitDisableMacros ? macroActions : noEdits()))});
}

RangeSelector SwitchCaseColonLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        return SM.getExpansionRange(Node->get<SwitchCase>()->getColonLoc());
    };
}

AST_MATCHER(SwitchCase, colonAndKeywordNotInMacroAndInMain) {
    (void)Builder;
    auto ColonLoc = Node.getColonLoc();
    auto KeywordLoc = Node.getKeywordLoc();
    const auto &SM = Finder->getASTContext().getSourceManager();
    return !ColonLoc.isMacroID() && !KeywordLoc.isMacroID() &&
           SM.isInMainFile(SM.getExpansionLoc(ColonLoc)) &&
           SM.isInMainFile(SM.getExpansionLoc(KeywordLoc));
}

auto handleSwitchCase() {
    auto matcher =
        switchStmt(
            inMainAndNotMacro,
            has(compoundStmt(
                has(switchCase(colonAndKeywordNotInMacroAndInMain())
                        .bind("firstcase")))),
            forEachSwitchCase(switchCase(colonAndKeywordNotInMacroAndInMain(),
                                         unless(equalsBoundNode("firstcase")))
                                  .bind("case")))
            .bind("stmt");
    auto actions = flatten(
        addMarkerAfter(SwitchCaseColonLoc("case"), cat("")),
        EmitDisableMacros ? edit(addDeleteMacroPre(insertBefore(
                                statementWithMacrosExpanded("case"), cat(""))))
                          : noEdits(),
        EmitDisableMacros
            ? edit(insertBefore(statementWithMacrosExpanded("case"),
                                cat("\n\n#endif\n\n")))
            : noEdits());
    return makeRule(matcher, actions);
}

RangeSelector SwitchStmtEndLoc(std::string ID) {
    return [ID](const clang::ast_matchers::MatchFinder::MatchResult &Result)
               -> Expected<CharSourceRange> {
        Expected<DynTypedNode> Node = getNode(Result.Nodes, ID);
        if (!Node) {
            llvm::outs() << "ERROR";
            return Node.takeError();
        }
        const auto &SM = *Result.SourceManager;
        const auto *SS = Node->get<SwitchStmt>();
        return SM.getExpansionRange(SS->getEndLoc());
    };
}

auto handleSwitch() {
    auto matcher =
        switchStmt(inMainAndNotMacro,
                   has(compoundStmt(
                       has(switchCase(colonAndKeywordNotInMacroAndInMain())
                               .bind("firstcase")))))
            .bind("stmt");
    auto actions =
        flatten(addMarkerAfter(SwitchCaseColonLoc("firstcase"), cat("")),
                EmitDisableMacros ? edit(insertBefore(SwitchStmtEndLoc("stmt"),
                                                      cat("\n\n#endif\n\n")))
                                  : noEdits(),
                EmitDisableMacros
                    ? edit(addDeleteMacroPre(insertBefore(
                          statementWithMacrosExpanded("firstcase"), cat(""))))
                    : noEdits());
    return makeRule(matcher, actions);
}

} // namespace

Instrumenter::Instrumenter(
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements)
    : FileToReplacements{FileToReplacements},
      Rules{{handleIfStmt(), Replacements, FileToNumberMarkerDecls},
            {handleWhile(), Replacements, FileToNumberMarkerDecls},
            {handleFor(), Replacements, FileToNumberMarkerDecls},
            {handleDoWhile(), Replacements, FileToNumberMarkerDecls},
            {handleSwitch(), Replacements, FileToNumberMarkerDecls},
            {handleSwitchCase(), Replacements, FileToNumberMarkerDecls}} {}

void Instrumenter::applyReplacements() {
    for (const auto &[File, NumberMarkerDecls] : FileToNumberMarkerDecls) {
        std::stringstream ss;
        auto gen = [i = 0]() mutable {
            return "void DCEMarker" + std::to_string(i++) + "_(void);\n";
        };
        std::generate_n(std::ostream_iterator<std::string>(ss),
                        NumberMarkerDecls, gen);
        auto Decls = ss.str();
        auto R = Replacement(File, 0, 0, Decls);
        if (auto Err = FileToReplacements[File].add(R))
            llvm_unreachable(llvm::toString(std::move(Err)).c_str());
    }

    for (auto Rit = Replacements.rbegin(); Rit != Replacements.rend(); ++Rit) {
        auto &R = *Rit;
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

void Instrumenter::registerMatchers(clang::ast_matchers::MatchFinder &Finder) {
    for (auto &Rule : Rules)
        Rule.registerMatchers(Finder);
}

namespace {
auto globalizeRule() {
    return makeRule(decl(anyOf(varDecl(hasGlobalStorage(), unless(isExtern()),
                                       unless(isStaticStorageClass())),
                               functionDecl(isDefinition(), unless(isMain()),
                                            unless(isStaticStorageClass()))),
                         isExpansionInMainFile())
                        .bind("global"),
                    insertBefore(node("global"), cat(" static ")));
}
} // namespace

GlobalStaticMaker::GlobalStaticMaker(
    std::map<std::string, clang::tooling::Replacements> &FileToReplacements)
    : Rule{globalizeRule(), FileToReplacements} {}

void GlobalStaticMaker::registerMatchers(
    clang::ast_matchers::MatchFinder &Finder) {
    Rule.registerMatchers(Finder);
}

} // namespace dead
