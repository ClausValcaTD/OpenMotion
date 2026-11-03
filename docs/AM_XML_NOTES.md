# Alight Motion XML notes

> **UNVERIFIED.** These notes were derived from reading how another open-source
> project parses Alight Motion project XML. They describe structure only and must
> be checked against real Alight Motion exports before the converter relies on
> them. No code, shader or asset from any other project is used.

## Scene

The root element carries the scene attributes:

- `title`
- `width`
- `height`
- `fps`
- `totalTime` — in milliseconds
- `bgcolor` — as `#AARRGGBB`

## Media entries

`<media>` entries describe source files; layers reference them. Attributes seen:
`uri`, `filename`, `title`, `src`, `type`, `width`, `height`, `orientation`.

## Layers

Layer elements seen: `layer`, `shape`, `drawing`, `text`, `image`, `video`,
`audio`, `null`, `group`, `embedscene`, `camera`.

Attributes seen: `id`, `label`/`name`, `startTime` and `endTime` (ms), `parent`
(the id of a parent layer), `hidden`, `fillVideo`/`fillImage`/`src`,
`mediaFillMode`, `s` (a shape name such as `".roundrect"`), `radius`.

A `size` property exists and appears to be stored as half-extents (the full size
is 2x the stored value).

## Transform

`<transform>` children seen: `location`, `scale`, `rotation`, `rotationX`,
`rotationY`, `skew`, `opacity`, `pivot`.

Each is either a constant (a `value` attribute holding one or more numbers) or
contains keyframes of the form `<kf t="..." v="..." e="..."/>`.

Keyframe time `t` is a fraction from 0 to 1 of the layer's own duration, **not**
seconds. The easing attribute `e` is stored on the keyframe the motion ARRIVES
at (the easing of the segment from keyframe `k` to `k+1` comes from keyframe
`k+1`). Our `.omproj` stores easing on the START keyframe, so the converter must
shift it by one position. Easing values seen include
`"cubicBezier x1 y1 x2 y2"`, step/hold, and named curves such as elastic and
bounce.

## Parenting and placement

A layer with a parent has its location relative to the parent (default `0,0,0`).
A root-level layer defaults to the scene center.

## Effects

Effects appear as `<effect>` children of a layer, identified by an id string.
Some effect definitions carry scripts or expressions: a converter must treat any
such content as plain data and must **NEVER** execute it.

## Bookmarks

Bookmarks (timeline markers) appear as `<bookmark t="ms" a="..."/>` in the scene.

## Colors

Colors are `#AARRGGBB`.

## Gaps between this and `.omproj` today

To be scheduled later:

- cubic-bezier and other easings
- timeline markers
- pivot/anchor
- skew
- parent/child layers
- groups
- trim/speed
- effects
