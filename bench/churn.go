package main

import "fmt"

type P struct{ A, B int64 }

func main() {
	keep := []*P{}
	var sum int64
	for i := int64(0); i < 5000000; i++ {
		p := &P{A: i, B: i + 1}
		q := p
		sum += q.A + p.B
		if i%1000 == 0 {
			keep = append(keep, p)
		}
	}
	fmt.Printf("sum %d kept %d\n", sum, len(keep))
}
