# Editor Tools (Geode)

Free helpers for the Geometry Dash editor, written from scratch. Four tools live in a small
button column inside the editor (position adjustable in the mod settings):

| Button  | What it does |
|---------|--------------|
| Scatter | Brush that sprays the **selected object** with random size / rotation while you drag. |
| Polygon | Tap to place points (drag to nudge the last one), then **Done** (or Enter). Builds an outline from stretched blocks; optional rounded corners and fill. |
| Lasso   | Draw a closed outline with your finger/mouse; the inside is filled with blocks. |
| Shot    | Saves a PNG of the level **without the editor UI** (desktop key: F2, configurable). |

| Art AI  | **Free AI Art Agent.** Select blocks, press it, describe the art in any language. |

The tools live in a drop-down: press **Tools** to open the list, pick one, and it closes again.

### Art AI (free)

Works like a professional art agent, with free services only:

1. **Shape** - reads the silhouette of the blocks you selected.
2. **Plan** - an AI (optional) turns your prompt into a detailed drawing brief.
3. **Drawing** - a free image model paints the picture.
4. **Trace** - it is reduced to a few colors and merged into as few blocks as possible.
5. **Place** - real blocks with their own color channels, clipped to your selection's shape.

The new blocks stay selected after drawing: press Delete to remove the whole drawing in one go.
Build the structure from **filled blocks** (not thin lines) for the best result.

You need a free account/key (no credit card). Set it up once in the mod settings:

| Service | What it does | Where |
|---------|--------------|-------|
| **Cloudflare FLUX** (default) | Best free *image* generator, ~100-170 images/day. Needs *Account ID* + *API token*: dashboard > AI > Workers AI > Use REST API. | <https://dash.cloudflare.com> |
| **Gemini** | Free language model; also used to plan prompts. | <https://aistudio.google.com> |
| **Groq / OpenRouter / Cerebras** | Free language models via *OpenAI-compatible*: fill URL, key, model. | <https://console.groq.com>, <https://openrouter.ai> |

Tip: put a Cloudflare token (drawing) **and** a Gemini or Groq key (planning) for the best results.

**Modes:** *Smart blocks (merged)* is the default and the best. *Shapes as real blocks* uses vector shapes drawn by Gemini/Groq.
*Recolor selected blocks* only changes colors. Settings let you pick detail (cell size), number of colors, block limit and overhang.
Free services have limits and can change; your prompt is sent to the service you choose.

Tap a tool again to turn it off. If nothing is selected, tools use the basic block (ID 1).
Every action is registered in the editor's undo list. All numbers (radius, thickness, fill size,
object limit, ...) are in the mod settings.

> Works best with square-ish blocks, since lines/fills are made by scaling the picked object.

## Build

No local setup needed: push to GitHub and open **Actions → Build Mod**. When it finishes,
download the `.geode` file from **Artifacts → Build Output** and drop it in your Geode `mods` folder.

Local build: install the Geode SDK + CLI and run `geode build`.
