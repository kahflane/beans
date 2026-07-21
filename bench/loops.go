package main

import "fmt"

func main() {
	sum := 0
	for i := 1; i <= 200000000; i++ {
		sum += i % 7
	}
	fmt.Println(sum)
}
