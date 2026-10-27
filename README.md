# Editor Tools (Geode)

Free helpers for the Geometry Dash editor, written from scratch. Four tools live in a small
button column inside the editor (position adjustable in the mod settings):

| Button  | What it does |
|---------|--------------|
| Scatter | Brush that sprays the **selected object** with random size / rotation while you drag. |
| Polygon | Tap to place points (drag to nudge the last one), then **Done** (or Enter). Builds an outline from stretched blocks; optional rounded corners and fill. |
| Lasso   | Draw a closed outline with your finger/mouse; the inside is filled with blocks. |
| Shot    | Saves a PNG of the level **without the editor UI** (desktop key: F2, configurable). |

| Art AI  | **Free AI Art Agent.** Select blocks, press it, describe the art in any language. The selection gets painted with the generated art, using real color channels (default 900+). |

### Art AI (free)

No payments and no server of ours: it talks to free third-party services directly from your game.

- **Pollinations (default):** no account, no key. Generates an image that is sampled onto your blocks.
- **Gemini:** paste a free API key from <https://aistudio.google.com> in the mod settings. The AI describes the art as simple shapes, which are painted locally.
- **Mode:** *Recolor selected blocks* (default) or *Draw new blocks in area* (finer detail).

Free services have limits and can change or go down; if one stops working, switch backend in the settings.
The prompt you type is sent to the chosen service.

Tap a tool again to turn it off. If nothing is selected, tools use the basic block (ID 1).
Every action is registered in the editor's undo list. All numbers (radius, thickness, fill size,
object limit, ...) are in the mod settings.

> Works best with square-ish blocks, since lines/fills are made by scaling the picked object.

## Build

No local setup needed: push to GitHub and open **Actions → Build Mod**. When it finishes,
download the `.geode` file from **Artifacts → Build Output** and drop it in your Geode `mods` folder.

Local build: install the Geode SDK + CLI and run `geode build`.
