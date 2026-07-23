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

func main() {
	sum := 0
	n, seed := arg(0, 200000000), arg(1, 1)
	for i := 1; i <= n; i++ {
		sum += (i + seed) % 7
	}
	fmt.Println(sum)
}
