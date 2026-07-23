unique class Handle {}
class ChildHandle extends Handle {}

fn main() {
    let first: ChildHandle = new ChildHandle()
    let copied: ChildHandle = first
}
