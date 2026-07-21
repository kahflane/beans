package main

import (
	"fmt"
	"strconv"
	"strings"
)

func main() {
	n := 800000

	parts := make([]string, 0, n)
	for i := 0; i < n; i++ {
		parts = append(parts, "item"+strconv.Itoa(i))
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
