CXX      := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2

SRC := src/token.cpp src/lexer.cpp src/parser.cpp src/ast_print.cpp src/checker.cpp src/codegen.cpp src/interp.cpp src/main.cpp
HDR := src/token.h src/lexer.h src/ast.h src/parser.h src/types.h src/checker.h src/value.h src/interp.h src/codegen.h
BIN := build/beansc

$(BIN): $(SRC) $(HDR)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

.PHONY: run clean
run: $(BIN)
	./$(BIN) parse examples/hello.b examples/tour.b

clean:
	rm -rf build
