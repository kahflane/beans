package main

import "fmt"

type Node struct {
	Left, Right *Node
}

func build(depth int) *Node {
	n := &Node{}
	if depth > 0 {
		n.Left = build(depth - 1)
		n.Right = build(depth - 1)
	}
	return n
}

func count(n *Node) int {
	total := 1
	if n.Left != nil {
		total += count(n.Left)
	}
	if n.Right != nil {
		total += count(n.Right)
	}
	return total
}

func main() {
	maxDepth := 14
	longLived := build(maxDepth)
	total := 0

	for d := 4; d <= maxDepth; d += 2 {
		iters := 1
		for k := maxDepth - d + 4; k > 0; k-- {
			iters *= 2
		}
		for i := 0; i < iters; i++ {
			total += count(build(d))
		}
	}

	fmt.Printf("trees %d long %d\n", total, count(longLived))
}
