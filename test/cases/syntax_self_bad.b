class Item {
    fn read(self) -> int { return 1 }
    static fn bad() -> Item { return self }
}
