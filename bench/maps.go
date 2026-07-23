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
	n, seed := arg(0, 400000), arg(1, 1)
	m := make(map[int]int, n) // size is known: preallocate, the Go idiom
	for i := 0; i < n; i++ {
		m[i+seed] = i*2 + seed
	}

	sum := 0
	for i := 0; i < n; i++ {
		sum += m[i+seed]
	}

	hits := 0
	for i := 0; i < n; i++ {
		if _, ok := m[i+seed]; ok {
			hits++
		}
		if _, ok := m[i+n+seed]; ok {
			hits++
		}
	}

	sn := n / 5
	sm := make(map[string]int, sn)
	for i := 0; i < sn; i++ {
		sm["key"+strconv.Itoa(seed)+"_"+strconv.Itoa(i)] = i + seed
	}
	ssum := 0
	kb := make([]byte, 0, 16)
	for i := 0; i < sn; i++ {
		kb = append(kb[:0], "key"...)
		kb = strconv.AppendInt(kb, int64(seed), 10)
		kb = append(kb, '_')
		kb = strconv.AppendInt(kb, int64(i), 10)
		ssum += sm[string(kb)] // the compiler elides this alloc for map reads
	}

	fmt.Printf("maps %d %d %d %d %d\n", sum, hits, ssum, len(m), len(sm))
}
