# The `.omproj` project format

`.omproj` is OpenMotion's project file: a JSON document describing a composition
and its layers. This document is derived from `src/project.cpp` and describes the
format version 1 exactly as the code reads and writes it.

The extension is `.omproj`, but the content is plain JSON — the loader parses it
with `nlohmann::json` and never inspects the filename.

## Versioning

- The top-level object **must** contain an integer `version`. It must equal `1`
  (the only supported version). A missing or non-integer `version`, or any other
  value, is rejected.
- There is no automatic migration; a future version would be a new value and new
  code.

## Top-level object

| Field | Type | Required | Default | Rules |
|---|---|---|---|---|
| `version` | integer | yes | — | Must be exactly `1`. |
| `name` | string | no | `""` | Free text. Ignored if not a string. |
| `width` | number | yes | — | Must be a finite integer-valued number, and positive. |
| `height` | number | yes | — | Must be a finite integer-valued number, and positive. |
| `fps` | number | yes | — | Must be present and `> 0`. |
| `duration_ms` | number | yes | — | Must be a finite integer-valued number and `>= 0`. |
| `layers` | array | no | `[]` | If present it must be an array; each element is a layer object. |

Numbers such as `width`/`height`/`duration_ms` may be written as JSON numbers
(e.g. `1280` or `1280.0`); the loader converts them to integers. Values too large
to fit an `int`/`int64_t`, or non-finite, are rejected rather than converted
(which would be undefined behaviour).

## Layer object

Each element of `layers` is an object. Layers are drawn bottom to top, so the
**first** layer is the backmost.

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `id` | string | yes | — | Must be present, a string, and non-empty. Must be unique across the project. |
| `type` | string | yes | — | One of the layer types below. |
| `name` | string | no | `""` | Display name. |
| `start_ms` | number | no | `0` | Inclusive start time on the timeline. |
| `end_ms` | number | no | `0` | Exclusive end time. Must be `>= start_ms`. |
| `transform` | object | no | see below | Animated transform. |
| `color` | string | no | `""` | `"#RRGGBB"`, used by `solid` and `rect`. |
| `text` | string | no | `""` | Reserved for `text` layers (not rendered yet). |
| `source` | string | no | `""` | File path, used by `image`, `video` and `audio`. |
| `width` | number | no | `0` | Used by `rect`, and as an explicit size override for `image`/`video`. |
| `height` | number | no | `0` | As above. |
| `in_ms` | number | no | `0` | **Audio only.** Offset into the source file. Must be `>= 0`. |
| `volume` | number | no | `1.0` | **Audio only.** Linear gain, must be finite and within `0`–`10`. |
| `blend` | string | no | `"normal"` | Blend mode. One of the names below. Ignored by `audio` layers. |

`start_ms` and `end_ms` are converted from numbers with the same range guard as
the top-level integers.

### Layer types

| `type` | Meaning | Fields it uses |
|---|---|---|
| `solid` | Full-canvas colour fill | `color`, `transform.opacity` |
| `rect` | Rotated/scaled rectangle | `color`, `width`, `height`, `transform` |
| `text` | Text | *(parsed; not rendered yet)* |
| `image` | Still image | `source`, `width`/`height` (optional override), `transform` |
| `video` | Video clip | `source`, `width`/`height` (optional override), `transform` |
| `audio` | Audio clip | `source`, `start_ms`, `end_ms`, `in_ms`, `volume` |

For `image` and `video` layers, if both `width` and `height` are greater than
zero they override the decoded size; otherwise the source's native size is used.
Transform, colour and size are ignored for `audio` layers.

### Blend modes

`blend` selects how a layer's colour is combined with what is already on the
canvas below it. It applies to `solid`, `rect`, `image` and `video` layers;
`audio` layers carry no pixels and ignore it. The canvas is always opaque, so
for a pixel with effective alpha `a = opacity * coverage * texture alpha` the
result is

```
result = (1 - a) * cb + a * blend_channel(mode, cb, cs)
```

per channel, where `cb` is the backdrop and `cs` the source (both in `[0, 1]`),
rounded to 8 bits. `normal` (`a = 1`, full coverage) is the original source-over
result and is byte-for-byte identical to a render with no blend field at all.

| `blend` | Formula for `blend_channel(mode, cb, cs)` |
|---|---|
| `normal` | `cs` |
| `multiply` | `cb * cs` |
| `screen` | `cb + cs - cb * cs` |
| `overlay` | `cb <= 0.5 ? 2*cb*cs : 1 - 2*(1-cb)*(1-cs)` |
| `exclusion` | `cb + cs - 2*cb*cs` |
| `color_burn` | `cb >= 1 ? 1 : (cs <= 0 ? 0 : 1 - min(1, (1-cb)/cs))` |
| `linear_dodge` | `min(1, cb + cs)` |

Every result is clamped to `[0, 1]`; a NaN input returns `cb`. An unknown blend
name is an error.

### Transform object

`transform` is optional. Any omitted property falls back to its default. Each of
its six fields is a **property**, described below.

| Field | Default constant |
|---|---|
| `x` | `0.0` |
| `y` | `0.0` |
| `scale_x` | `1.0` |
| `scale_y` | `1.0` |
| `rotation` | `0.0` |
| `opacity` | `1.0` |

`rotation` is in degrees, clockwise. `x`/`y` are the centre of the layer in
canvas pixels. `opacity` is clamped to `[0, 1]` when rendering; values outside
that range are accepted by the parser (for example `5.0` is stored as-is).

### Property

A property is either a **constant** or an **animated track**:

- **Constant** — a plain JSON number, e.g. `"opacity": 0.75`.
- **Animated** — an object with a `keyframes` array:

  ```json
  "x": {
    "keyframes": [
      { "t": 0,    "v": 0.0,   "easing": "linear" },
      { "t": 1500, "v": 400.0, "easing": "ease_in_out" },
      { "t": 3000, "v": 800.0, "easing": "linear" }
    ]
  }
  ```

  Anything that is neither a number nor an object with a `keyframes` array is an
  error.

#### Keyframe

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `t` | number | yes | — | Time in milliseconds. Must be integer-valued and in range. |
| `v` | number | yes | — | Value at that time. |
| `easing` | string | no | `"linear"` | The easing of the segment **starting** at this keyframe. |
| `bezier` | array of 4 numbers | see below | `[0, 0, 1, 1]` | CSS cubic-bezier control points `[x1, y1, x2, y2]`. Required when `easing` is `"cubic_bezier"`, ignored otherwise. |

Keyframes must be **sorted by `t`** (non-decreasing); out-of-order keyframes are
an error. Two keyframes may share the same `t`; the evaluator then holds the
earlier value for that zero-length span.

#### Easing names

`easing` is one of exactly these strings:

| Name | Curve |
|---|---|
| `linear` | `t` |
| `ease_in` | `t³` |
| `ease_out` | `1 − (1 − t)³` |
| `ease_in_out` | `4t³` for `t < 0.5`, else `1 − (−2t + 2)³ / 2` |
| `hold` | step: `0` until `t = 1`, then `1` |
| `cubic_bezier` | a CSS `cubic-bezier(x1, y1, x2, y2)` curve from the `bezier` array |

An unknown easing string is an error. The easing on a keyframe governs the
segment from that keyframe to the next one.

`cubic_bezier` reproduces CSS `cubic-bezier()`. The four numbers are the two
control points `x1 y1 x2 y2`, in the same order as CSS. `x1` and `x2` must be
within `[0, 1]`; `y1` and `y2` may leave `[0, 1]` (that is what produces an
overshoot), up to an absolute value of `10`. Evaluation solves the curve for
the time progress `u` and returns the eased value, which is **not** clamped.
Alight Motion exports mostly use this easing (see `docs/AM_XML_NOTES.md`).

## Serialization rules

`to_json_string` pretty-prints with an indent of 2 and `save_to_file` appends a
trailing newline. The writer always emits every field of every layer — including
`name`, `color`, `text`, `source`, `width` and `height` even when empty or zero —
**except** `in_ms` and `volume`, which are written **only for `audio` layers**
and omitted for every other type, and `blend`, which is written **only when it
is not `normal`** and omitted otherwise. The transform is always written with
all six properties; a property is written as a bare number when it has no
keyframes, and as a `{"keyframes": [...]}` object otherwise.

The `bezier` array is written **only** for `cubic_bezier` keyframes. Every other
easing serializes exactly as before this field existed, so existing files stay
byte-identical on a load → save round trip.

Because the writer always emits the full set of layer fields, a load → save
round trip is stable: loading the saved file yields an equal project.

## Validation rules (all return `ProjectError`, never throw)

- Top level must be a JSON object.
- `version` must be present, an integer, and equal to `1`.
- `width`, `height` must be present, integer-valued, and positive.
- `fps` must be present and `> 0`.
- `duration_ms` must be present, integer-valued, and `>= 0`.
- `layers`, if present, must be an array.
- Each layer must be an object with a non-empty string `id` and a known string
  `type`.
- Layer ids must be unique.
- `start_ms`/`end_ms`/`in_ms` must be in range; `in_ms >= 0`.
- `end_ms >= start_ms`.
- `volume` must be finite and within `[0, 10]`.
- `blend`, if present, must be a string naming a known blend mode.
- A property must be a number or an object with a `keyframes` array.
- Each keyframe must be an object with numeric `t` and `v`; `t` must be in range.
- Keyframes must be sorted by `t`.
- `easing`, if present, must be a known name.
- A `cubic_bezier` keyframe must carry a `bezier` array of exactly four numbers.
- `bezier` values must be finite; `x1` and `x2` must be within `[0, 1]`; `y1`
  and `y2` must satisfy `|y| <= 10`.
- `bezier` on a non-`cubic_bezier` keyframe is ignored (not an error).

Malformed JSON is reported as a parse error; anything that throws internally is
caught and turned into a `ProjectError`.

## Example

The bundled `examples/hello.omproj`, annotated:

```jsonc
{
  "version": 1,                 // format version, must be 1
  "name": "Hello OpenMotion",   // free-text name
  "width": 1280,                // canvas size
  "height": 720,
  "fps": 30.0,                  // frames per second
  "duration_ms": 3000,          // 3 seconds
  "layers": [                   // bottom (first) to top (last)
    {
      "id": "bg",               // unique id
      "type": "solid",          // full-canvas colour fill
      "name": "Background",
      "start_ms": 0,            // visible for the whole timeline
      "end_ms": 3000,
      "transform": {            // all-constant transform
        "x": 0.0, "y": 0.0,
        "scale_x": 1.0, "scale_y": 1.0,
        "rotation": 0.0, "opacity": 1.0
      },
      "color": "#101018",       // dark background
      "text": "",               // unused for solid
      "source": "",
      "width": 0.0,             // unused for solid
      "height": 0.0
    },
    {
      "id": "box",
      "type": "rect",           // 200x120 rectangle
      "name": "Box",
      "start_ms": 0,
      "end_ms": 3000,
      "transform": {
        "x": {                  // animated: x moves 0 -> 400 -> 800
          "keyframes": [
            { "t": 0,    "v": 0.0,   "easing": "linear" },
            { "t": 1500, "v": 400.0, "easing": "ease_in_out" },
            { "t": 3000, "v": 800.0, "easing": "linear" }
          ]
        },
        "y": 360.0,             // constant vertical centre
        "scale_x": 1.0,
        "scale_y": 1.0,
        "rotation": 0.0,
        "opacity": 0.75         // 75% opaque
      },
      "color": "#e04030",
      "text": "",
      "source": "",
      "width": 200.0,           // rectangle size
      "height": 120.0
    }
  ]
}
```

An audio layer, for reference (not part of the example file):

```jsonc
{
  "id": "music",
  "type": "audio",
  "name": "Music",
  "start_ms": 500,       // begins 0.5 s into the timeline
  "end_ms": 3000,
  "source": "music.wav", // resolved relative to the project file
  "in_ms": 1000,         // skip the first second of the source
  "volume": 0.8          // 80% gain
}
```
