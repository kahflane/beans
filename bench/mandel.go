package main

import "fmt"

func main() {
	w := 1800
	h := 1800
	maxIter := 100
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
