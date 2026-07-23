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
	size, seed := arg(0, 1800), arg(1, 1)
	w := size
	h := size
	maxIter := 80 + seed%41
	inside := 0

	for y := 0; y < h; y++ {
		ci := float64(y)*2.0/float64(h) - 1.0
		for x := 0; x < w; x++ {
			cr := float64(x)*3.0/float64(w) - 2.0
			zr := 0.0
			zi := 0.0
			i := 0
			for ; i < maxIter; i++ {
				t := zr*zr - zi*zi + cr
				zi = 2.0*zr*zi + ci
				zr = t
				if zr*zr+zi*zi > 4.0 {
					break
				}
			}
			if i == maxIter {
				inside++
			}
		}
	}

	fmt.Printf("mandel %d\n", inside)
}
