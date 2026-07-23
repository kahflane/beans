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

type Shape interface{ Area() float64 }

type Circle struct{ R float64 }

func (c Circle) Area() float64 { return 3.14159265 * c.R * c.R }

type Square struct{ S float64 }

func (s Square) Area() float64 { return s.S * s.S }

func main() {
	n, seed := arg(0, 100000000), arg(1, 1)
	tweak := float64(seed%7) / 100.0
	shapeCount := 8 + seed%8
	shapes := make([]Shape, 0, shapeCount)
	for i := 0; i < shapeCount; i++ {
		if i%2 == 0 {
			shapes = append(shapes, Circle{R: 1.5 + tweak})
		} else {
			shapes = append(shapes, Square{S: 2.0 + tweak})
		}
	}
	total := 0.0
	for i := 0; i < n/shapeCount; i++ {
		for _, s := range shapes {
			total += s.Area()
		}
	}
	fmt.Println(int64(total + 0.5))
}
