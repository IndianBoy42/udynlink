# nanopb runtime (vendored)

nanopb 0.4.9.1 (https://github.com/nanopb/nanopb) — C runtime only, for
`scripts/proto2module` and the `tests/test-protobuf-module` QEMU test.

Why vendored: the pip package (`pip install nanopb`) ships only the protoc
generator plugin, not the C runtime headers/sources that modules and the host
firmware compile against. Keeping the runtime in-tree makes the pipeline and
tests self-contained.

Contents (copied from the nanopb 0.4.9.1 source tree):

| File             | Source location              |
|------------------|------------------------------|
| pb.h             | nanopb root                  |
| pb_common.h/.c   | nanopb root                  |
| pb_encode.h/.c   | nanopb root                  |
| pb_decode.h/.c   | nanopb root                  |
| nanopb.proto     | generator/proto/nanopb.proto |
| LICENSE.txt      | nanopb root (BSD-3-Clause)   |

To update: download the matching nanopb release, copy the files above into
this directory, and regenerate `tests/test-protobuf-module/sensor.pb.{c,h}`
with `scripts/proto2module`.

The `protoc-gen-nanopb` generator plugin is *not* vendored — install it with
`uv pip install nanopb` (it must match the runtime version).
