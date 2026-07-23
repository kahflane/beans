@move_only class Handle {}
class Child : Handle {}
fn old<T: Clone>(value: T) -> T { return value }
