# Golden Files

These videos come from the reference build at commit `ffd9297`. `golden_test.cpp` decodes each of them and checks
that the result equals `input.bin`, and renders the packets of each `.mkv` again to check that every frame matches the
reference byte for byte. Never regenerate them with a newer build: they prove that newer builds stay compatible.

## How they were made

`input.bin` holds 1000 bytes, byte `index` being `(index * 37 + 11) % 256`. The reference CLI encoded it with these
commands:

```sh
media_storage encode --input input.bin --output plain-crc32.mkv
media_storage encode --input input.bin --output plain-xxhash.mkv --hash xxhash
media_storage encode --input input.bin --output encrypted-crc32.mkv --encrypt --password golden-password
media_storage stream-encode --input input.bin --url plain-crc32.flv
media_storage stream-encode --input input.bin --url encrypted-xxhash.flv --encrypt --password golden-password --hash xxhash
```

The reference CLI decoded every video back to `input.bin`.

## Checksums

| File | SHA-256 |
|---|---|
| `input.bin` | `57799de80e3dd6e2ac4d40c41a150d1662f7f87d0d994776a2fdc37c39b0ea4e` |
| `plain-crc32.mkv` | `3439b57b0b5c238b88b3629754e254df1447e1caa1279ca1ab55aaf46b7fbd90` |
| `plain-xxhash.mkv` | `bb547b6b47fb77ba870fb52ce5b3534b927638de0bb39eaea9aa0a291b355876` |
| `encrypted-crc32.mkv` | `6d3bf9e5eed22ac34827c9b41ae0d6df8eb276a92f79ae1677c7c7e06a8a22a9` |
| `plain-crc32.flv` | `ce0810ad7f9764c9645ffe5efa42437e64347261cee8e0922b06622f4e97b46d` |
| `encrypted-xxhash.flv` | `beccdaa729da2062f284b6f29ef427ce683a327ace7a8050a0c2aa52955b3074` |

The `.flv` files hold lossy H.264, so `golden_test.cpp` only decodes them; their frames depend on the encoder's
thread count.
