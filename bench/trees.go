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

type Node struct {
	Left, Right *Node
	Value       int
}

func build(depth, seed int) *Node {
	n := &Node{Value: depth + seed}
	if depth > 0 {
		n.Left = build(depth-1, seed)
		n.Right = build(depth-1, seed)
	}
	return n
}

func count(n *Node) int {
	total := 1 + n.Value
	if n.Left != nil {
		total += count(n.Left)
	}
	if n.Right != nil {
		total += count(n.Right)
	}
	return total
}

func main() {
	maxDepth, seed := arg(0, 14), arg(1, 1)
	longLived := build(maxDepth, seed)
	total := 0

	for d := 4; d <= maxDepth; d += 2 {
		iters := 1
		for k := maxDepth - d + 4; k > 0; k-- {
			iters *= 2
		}
		for i := 0; i < iters; i++ {
			total += count(build(d, seed))
		}
	}

	fmt.Printf("trees %d long %d\n", total, count(longLived))
}
