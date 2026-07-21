package main

import "fmt"

type Shape interface{ Area() float64 }

type Circle struct{ R float64 }

func (c Circle) Area() float64 { return 3.14159265 * c.R * c.R }

type Square struct{ S float64 }

func (s Square) Area() float64 { return s.S * s.S }

func main() {
	shapes := []Shape{Circle{R: 1.5}, Square{S: 2.0}}
	total := 0.0
	for i := 0; i < 50000000; i++ {
		for _, s := range shapes {
			total += s.Area()
		}
	}
	fmt.Println(int64(total + 0.5))
}
