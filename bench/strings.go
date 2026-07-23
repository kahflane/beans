package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
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
	n, seed := arg(0, 2000000), arg(1, 1)

	parts := make([]string, 0, n)
	for i := 0; i < n; i++ {
		parts = append(parts, "item"+strconv.Itoa(seed)+"_"+strconv.Itoa(i))
	}

	joined := strings.Join(parts, ",")
	up := strings.ToUpper(joined)
	back := strings.Split(joined, ",")

	hits := 0
	for _, p := range back {
		if strings.Contains(p, "999") {
			hits++
		}
	}

	swapped := strings.ReplaceAll(joined, "item", "row")

	fmt.Printf("strings %d %d %d %d %d\n",
		len(joined), len(up), len(back), hits, len(swapped))
}
