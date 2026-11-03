# Video Deck artwork

`save-menu.bin` is the background and ten selected-control states extracted from
the user-supplied **Gex save menu** screenshots. The original screenshots are not
required to build or run the port. The game artwork remains the property of its
rights holders; it is not newly authored project artwork.

To re-extract from that screenshot set (requires Pillow):

```sh
python tools/save_menu_assets.py extract "/path/to/Gex save menu" assets/save-menu.bin
```

The extractor removes pillarboxing, reduces the image to 320x240, shares a palette
with dedicated bright red/green ramps, and stores the ten selections as deltas
from an unselected background. The LCD numbers are replaced at runtime with the
selected decks' actual progression. All other cabinet artwork comes from the
screenshots.

CMake embeds the pack into desktop/Android binaries using the same script's
`embed` operation, which needs only Python's standard library. No extra file
beside the executable, APK asset copy, or runtime image decoder is needed.

Options needs no extracted artwork: it uses the original main-menu font and
selection animation through the game's built-in text/CEL renderers, over
full-viewport animated main-menu TV static.
