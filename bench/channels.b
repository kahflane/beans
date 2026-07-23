// One producer and one consumer over a bounded channel.
import std.io
import std.os
import std.thread

fn produce(channel: Channel<int>, n: int, seed: int) -> int {
    var i: int = 0
    for i < n {
        channel.send((i * 17 + seed) % 1_000_003)
        i += 1
    }
    channel.close()
    return n
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(1_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    let channel: Channel<int> = new Channel(1024)
    let producer: Thread<int> = thread.spawn(fn() -> int { return produce(channel, n, seed) })
    var checksum: int = 0
    var i: int = 0
    for i < n {
        checksum += channel.recv().or(-1)
        i += 1
    }
    let sent: int = producer.join()
    io.println("channels {checksum} {sent} {channel.recv().is_none()}")
}
