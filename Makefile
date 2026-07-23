CXX      := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2 -pthread

SRC := src/token.cpp src/lexer.cpp src/parser.cpp src/ast_print.cpp src/loader.cpp src/mir.cpp src/c_abi.cpp src/checker.cpp src/builtins.cpp src/codegen.cpp src/interp.cpp src/lsp.cpp src/main.cpp
HDR := src/token.h src/lexer.h src/ast.h src/parser.h src/types.h src/target.h src/mir.h src/hir.h src/c_abi.h src/loader.h src/checker.h src/value.h src/builtins.h src/interp.h src/codegen.h src/lsp.h
BIN := build/beansc
RUNTIME_SRC := runtime/beans_rt.c
RUNTIME_COPY := build/beans_rt.c
.DEFAULT_GOAL := $(BIN)

$(RUNTIME_COPY): $(RUNTIME_SRC)
	@mkdir -p build
	cp $(RUNTIME_SRC) $(RUNTIME_COPY)

$(BIN): $(SRC) $(HDR) $(RUNTIME_COPY)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

.PHONY: run clean test test-sanitize bench-quick bench-full bench-verify bench-profile
run: $(BIN)
	./$(BIN) parse examples/hello.b examples/tour.b

test: $(BIN)
	./test/differential.sh
	./test/numerics.sh
	./test/moves.sh
	./test/maps.sh
	./test/traits.sh
	bash ./test/syntax_v07.sh
	bash ./test/unsafe.sh
	./test/fixed_arrays.sh
	./test/raw_slices.sh
	./test/c_layout_structs.sh
	./test/c_layout_unions.sh
	./test/c_layout_c_abi.sh
	./test/c_callbacks.sh
	bash ./test/closure_captures.sh
	./test/stdlib_source.sh
	./test/lsp_probe.sh
	./test/fs_source.sh
	./test/reader_source.sh
	./test/inline_options.sh
	./test/inline_results.sh
	bash ./test/wide_lists.sh
	bash ./test/wide_maps.sh
	bash ./test/wide_enums.sh
	bash ./test/wide_owners.sh
	bash ./test/wide_sync.sh
	bash ./test/wide_concurrency.sh

test-sanitize: $(BIN)
	bash ./test/sanitize.sh

bench-quick: $(BIN)
	./bench/run.sh quick

bench-full: $(BIN)
	./bench/run.sh full

bench-verify: $(BIN)
	./bench/run.sh verify

bench-profile: $(BIN)
	@test -n "$(NAME)" || { echo "usage: make bench-profile NAME=trees"; exit 2; }
	./bench/profile.sh "$(NAME)"

clean:
	rm -rf build
