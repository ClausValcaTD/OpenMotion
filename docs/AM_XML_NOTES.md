# Alight Motion XML notes (structure only)

> Verified against five real exports produced by Alight Motion 5.0.275
> (Android, file-format version `ffver 106`). Items tagged `[verified]` were
> checked numerically against those files; `[assumed]` items were not. Other
> versions/platforms may differ.

## Root

`[verified]` `<scene>` attributes: `title`, `width`, `height`, `exportWidth`,
`exportHeight`, `precompose`, `bgcolor` (`#AARRGGBB`, transparent in the
samples), `totalTime` (ms), `fps`, `modifiedTime`, `amver`, `ffver`, `am`,
`amplatform`, `retime`, `retimeAdaptFPS`. A leading XML comment records app
name/version/export time.

`[verified]` Children of `<scene>` are layers, then possibly
`<bookmark t="ms"/>` markers (`t` in milliseconds, NOT a fraction). Layers
appear in file order, `[assumed]` bottom to top (the full-frame background is
first, a full-frame effects layer is last).

## Layer elements (all `[verified]` unless noted)

`shape`, `text`, `embedScene`, `nullobj` (`type="perspective"` seen).

Common attributes: `id`, `label`, `startTime`, `endTime` (ms), `fillType`,
`mediaFillMode`, `parent` (id), `hidden`, `clippingMask`, `blending`.

`fillType` values: `color`, `gradient`, `media`, `none`, `intrinsic`
(embedScene).

`blending` values seen: `screen`, `multiply`, `exclude`, `color-burn`,
`linear-dodge`, `mask`.

**shape:** `s` = `.rect` | `.roundrect` | `.circle` | `.line`, or a
`<path d="...">` using M/L/Z commands plus `<path-stroke direction join
end-size>` with `<color>` and `<size>`. `<fillColor value="#AARRGGBB"/>`.
`<gradient type="linear" startColor endColor start end/>` with normalized
coordinates. `<property name="size" type="vec2">` holds HALF-extents: a rect's
full size = 2 x size x scale; a circle's radius = size x scale. Images are
shapes with `fillType="media"` and `fillImage="content://..."` (an Android
content URI that is meaningless off-device). The only portable identifier is
the layer label, which is the file name; a converter must look for that file
next to the XML. No `<media>` element exists in these files. `[assumed]`
video/audio are represented similarly; none were present.

**text:** attributes `size` (font size), `font`
(`"googlefonts?name=...&weight=..."`), `wrapWidth`, `align`; text lives in a
`<content>` child.

**embedScene:** a precomposition. Contains a complete nested `<scene>` with its
own size/fps/totalTime, plus its own `<transform>` and `<effect>` children;
attribute `outTime` equals the layer duration. `[assumed]` mapping between the
layer time and the nested scene time is not yet understood. 11 of the 16 scenes
in the samples are nested.

## Transform and keyframes

`<transform>` children: `location` (3 numbers), `scale` (2 numbers, a
multiplier of size), `pivot` (2 numbers), `rotation` (scalar), `opacity`
(0..1). Each is either a `value` attribute or a list of `<kf t v e>` children.

`[verified]` Keyframes are NOT stored in time order; sort by `t`.

`[verified]` `t` is a fraction of `(endTime - startTime)` of the owning layer;
`t` may be slightly negative.

`[verified]` Converted to frames at the scene fps, keyframe times sit about
half a frame after the frame boundary; recommended mapping:
`frame = round(t * duration_ms * fps / 1000 - 0.5)`, clamped to >= 0.

`[verified]` The easing attribute `e` belongs to the keyframe the motion
ARRIVES at. Our `.omproj` stores easing on the START keyframe, so the converter
must shift it by one position after sorting. The first keyframe's `e` is
irrelevant. `[assumed]` a missing `e` means linear.

Easing grammar `[verified]`: `"cubicBezier x1 y1 x2 y2"` (same order as CSS;
y values may go outside 0..1, giving overshoot; about 70% of uses) and
`"elastic p1 p2 p3 p4"` (four parameters, semantics UNKNOWN).

Parenting: a child's location is relative to its parent. `[verified]` with an
unrotated, unscaled parent the child's world position is the sum of the
locations. `[assumed/unknown]` the exact formula when the parent has rotation,
scale or a pivot.

Colors are `#AARRGGBB`.

## Effects

`<effect id="com.alightcreative.…" locallyApplied hidden?>` with
`<property name type value/>` children; types: `float`, `int`, `bool`, `vec2`,
`vec3`, `color`, `quat`. A property can instead contain `<kf>` children
(animated parameter). Effect ids seen with counts: lift 8, gaussianblur 6,
solidcolor 5, invert 5, hueshift 4, motionblur4 3, texttransform 3, noise3 2,
and one each of bendwarp, glow, softglow, lightglow, stripes, brightcont2,
satvib, sharpen, inner-glow, exposure, repeat.grid, textprogress, extrude. A
converter must treat all effect content as plain data and must NEVER execute
anything from a file.

## What OpenMotion needs before a faithful import

Shapes (circle, rounded rect, path, gradient), blend modes and masks, parent
hierarchy with pivot, cubic-bezier and elastic easing, compositions (precomps),
text (with fonts), and clean-room implementations of the effects above. Until
then the converter should import best-effort and print a report of everything
it skipped.

## Rules for this repository

Do not commit exported files, images or text from other people's projects. Test
fixtures are small hand-written XML files that mimic the structure.
