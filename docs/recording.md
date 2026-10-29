# Recordings

A recording holds the messages of several topics in one file, in the order
they arrived, so they can be replayed later or read by a program.
`puppet_master_topic record` writes them, `replay` publishes them again, and
`info` summarizes them; see [Topic Tool](launcher.md#topic-tool).

```bash
puppet_master_topic record -o drive.pmrec --system system.json --duration-ms 60000
puppet_master_topic info drive.pmrec
puppet_master_topic replay drive.pmrec /vehicle/imu /vehicle/wheel_speed --rate 2
```

## Recording

`record` subscribes to the topics named, or with none named, to every
shared-memory topic found: those with a segment on this host, or with
`--system FILE`, those the manifest's nodes declare. It stops after
`--count N` messages in total, after `--duration-ms N`, or on Ctrl+C, and
prints how many messages of each topic it recorded and lost.

The recorder is an ordinary reader that queues up to one ring per topic. It
never slows a writer down. When it falls behind by more than a ring, the
oldest messages are overwritten and counted as lost. A larger `slot_count`
in the deployment gives it more room.

Each message is stored with its payload, the writer's sequence number, and
its publish time relative to the start of the recording. Each topic is
stored with its name, its message descriptor (type, encoding, and version),
and the shared-memory domain and ring size it was recorded from.

The recorder flushes the file every 200 ms. If it is killed, the file ends
inside a record at worst; readers stop there and report the recording as cut
off, and everything before is kept.

## Replaying

`replay` publishes the messages of every topic, or of the topics named, with
the descriptor, domain, and ring size each was recorded with. `--domain N`
publishes to another domain instead, for example to replay next to a live
system without disturbing it.

Messages keep their recorded spacing, measured from the first message
replayed. `--rate 2` replays twice as fast, `--rate 0.5` at half speed, and
`--rate 0` publishes everything at once. Readers see new sequence numbers and
publish times, as from any other writer.

Stop the system's own writers of a topic before replaying it, or readers
receive both.

## Reading In C++

`RecordingReader` and `RecordingWriter` in
`puppet_master/transport/recording.h` are in the core library and need no
transport:

```cpp
auto recording = transport::RecordingReader::Open("drive.pmrec");
if (!recording.ok()) { /* missing, not a recording, or a newer format */ }
while (true) {
    auto next = recording.value()->Next();
    if (!next.ok()) { /* corrupt */ }
    if (!next.value()) {
        break;  // the end; truncated() tells whether it was cut off
    }
    const auto& message = *next.value();
    const auto* topic = recording.value()->FindTopic(message.topic);
    // topic->name, topic->descriptor, message.time, message.payload
}
```

Topics are declared before their first message, so `FindTopic` always finds
the topic of a message just read. `topics()` grows as the file is read.

## File Format

All integers are little-endian. A string is a `u32` byte count followed by
the bytes.

| Part | Contents |
|---|---|
| file header | `"PMREC001"`, `u32` format version (1), `u32` reserved (0) |
| record | `u32` kind, `u32` body size, body |
| topic body (kind 1) | `u32` id, string name, string type, string encoding, `u32` message version, `u32` parameter count, then each parameter as two strings, key and value |
| message body (kind 2) | `u32` topic id, `i64` time in ns since the start, `u64` sequence, payload (the rest of the body) |

Topic ids start at 1, in the order topics are declared. Readers skip records
of unknown kinds, so later versions can add kinds without breaking them. A
change to an existing record raises the format version, which older readers
refuse.

The recorder stores these topic parameters: `transport` (`shm`),
`domain_id`, `slot_payload_size`, and `slot_count`.
