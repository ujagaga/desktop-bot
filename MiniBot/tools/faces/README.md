# Editable face images

Replace these PNGs to change the robot artwork. Only the first two filename characters determine the `lcd face` ID (00–15).
The rest of the name is arbitrary: `01_happy.png` and `01_custom.png` both map to ID 1.
Provide exactly one PNG per ID; missing or duplicate IDs are rejected.

| ID | Source file |
| --- | --- |
| 0 | [00_neutral.png](00_neutral.png) |
| 1 | [01_happy.png](01_happy.png) |
| 2 | [02_sad.png](02_sad.png) |
| 3 | [03_excited.png](03_excited.png) |
| 4 | [04_surprised.png](04_surprised.png) |
| 5 | [05_wink.png](05_wink.png) |
| 6 | [06_angry.png](06_angry.png) |
| 7 | [07_confused.png](07_confused.png) |
| 8 | [08_sleepy.png](08_sleepy.png) |
| 9 | [09_laughing.png](09_laughing.png) |
| 10 | [10_blushing.png](10_blushing.png) |
| 11 | [11_worried.png](11_worried.png) |
| 12 | [12_skeptical.png](12_skeptical.png) |
| 13 | [13_playful.png](13_playful.png) |
| 14 | [14_cool.png](14_cool.png) |
| 15 | [15_shocked.png](15_shocked.png) |

From the repository root, regenerate and build:

```sh
python3 tools/import_faces.py
tools/build_s3.sh
```

The importer defaults to a maximum dimension of 240 pixels. It preserves aspect
ratio and never enlarges smaller images. For lower resolution, use `--size 120`,
or override one face with `--face-size 01=96`. Transparency becomes black.
Images are centered on the LCD at their resulting native dimensions.

Output: `ESP32_S3/face_assets.h`. Optional rendered previews can be generated with
`--previews /tmp/face-previews`. No `assets` folder is needed or created. Source PNGs
are never overwritten by the importer. Install the rebuilt firmware to see changes.
