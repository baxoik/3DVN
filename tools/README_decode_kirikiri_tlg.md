# Decode selected KiriKiri TLG images

`decode_kirikiri_tlg.py` uses a locally installed
[arc_unpacker](https://github.com/vn-tools/arc_unpacker) executable; it does
not include or download the decoder. Python 3 and Pillow are required.
It accepts extracted TLG0-wrapped, TLG5, and TLG6 images, not archives.
Only 3- and 4-channel images are supported.

```sh
python3 tools/decode_kirikiri_tlg.py \
  --decoder /path/to/arc_unpacker --output /path/to/new-directory \
  /path/to/extracted/first.tlg /path/to/extracted/second.tlg
```

Provide any number of selected input files (shell glob expansion also works).
The output parent must exist, and the output directory must not exist. Source
files are copied into a staging directory and never modified. The tool rejects
case-insensitive output name collisions, decoding failures, invalid PNGs,
dimension mismatches, and missing alpha for four-channel sources. It publishes
the batch only after all images validate. PNG names retain the original stems.

The images retain their original resolution and alpha channel; they are not
resized or composed for the engine. TLG0 tags and other game-specific placement
or layer information are not exported. A successful decode is not proof that
the image is a complete scene or that every TLG variant is supported. Decoding
is local; keep proprietary game assets private.
