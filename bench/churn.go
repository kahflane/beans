package main

import (
	"fmt"
	"os"
	"strconv"
)

func arg(at, fallback int64) int64 {
	if len(os.Args) <= int(at)+1 {
		return fallback
	}
	n, err := strconv.ParseInt(os.Args[at+1], 10, 64)
	if err != nil {
		return fallback
	}
	return n
}

type P struct{ A, B int64 }

func main() {
	n, seed := arg(0, 5000000), arg(1, 1)
	keep := make([]*P, 0, n/1000+1)
	var sum int64
	for i := int64(0); i < n; i++ {
		p := &P{A: i + seed, B: i + seed + 1}
		q := p
		sum += q.A + p.B
		if i%1000 == 0 {
			keep = append(keep, p)
		}
	}
	fmt.Printf("sum %d kept %d\n", sum, len(keep))
}
