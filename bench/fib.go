package main

import (
	"fmt"
	"os"
	"strconv"
)

func arg(at, fallback int) int {
	if len(os.Args) <= at+1 {
		return fallback
	}
	n, err := strconv.Atoi(os.Args[at+1])
	if err != nil {
		return fallback
	}
	return n
}

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	n, seed := arg(0, 40), arg(1, 1)
	fmt.Println(fib(n) + seed - seed)
}
