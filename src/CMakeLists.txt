add_library(DeadInstrumentlib DeadInstrumenter.cpp Common.cpp ValueRangeTagger.cpp)
target_include_directories(DeadInstrumentlib PUBLIC ${CLANG_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR})

if(CLANG_LINK_CLANG_DYLIB)
    target_link_libraries(DeadInstrumentlib PUBLIC LLVM)
    clang_target_link_libraries(DeadInstrumentlib PUBLIC)
else()
    llvm_map_components_to_libnames(llvm_libs support core)
    target_link_libraries(DeadInstrumentlib PUBLIC ${llvm_libs}
                                                   clangASTMatchers
                                                   clangTransformer
                                                   clangTooling)
endif(CLANG_LINK_CLANG_DYLIB)


add_subdirectory(tool)
