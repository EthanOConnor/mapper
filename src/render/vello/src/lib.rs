use std::num::{NonZeroIsize, NonZeroU32};
use std::sync::{Arc, Mutex};
use std::thread;

use flume::{Receiver, Sender, TryRecvError, TrySendError};
use raw_window_handle::{
    AndroidDisplayHandle, AndroidNdkWindowHandle, AppKitDisplayHandle, AppKitWindowHandle,
    RawDisplayHandle, RawWindowHandle, UiKitDisplayHandle, UiKitWindowHandle, WaylandDisplayHandle,
    WaylandWindowHandle, Win32WindowHandle, WindowsDisplayHandle, XcbDisplayHandle,
    XcbWindowHandle,
};
use vello::kurbo::{Affine, BezPath, Cap, Ellipse, Join, Rect, Stroke};
use vello::peniko::{
    BlendMode, Blob, Color, ColorStop, Compose, Extend, Fill, Gradient, ImageAlphaType, ImageBrush,
    ImageData, ImageFormat, ImageQuality, ImageSampler, Mix,
};
use vello::wgpu;
use vello::wgpu::util::TextureBlitter;
use vello::{AaConfig, AaSupport, RenderParams, Renderer as VelloRenderer};
use vello::{RendererOptions, Scene};

#[cxx::bridge(namespace = "OpenOrienteering::render::vello_ffi")]
mod ffi {
    #[derive(Clone, Copy, Default)]
    struct SurfaceState {
        sequence: u64,
        phase: u8,
        platform: u8,
        window: u64,
        display: u64,
        width: u32,
        height: u32,
    }

    #[derive(Clone, Copy, Default)]
    struct Transform {
        m11: f64,
        m12: f64,
        m21: f64,
        m22: f64,
        dx: f64,
        dy: f64,
    }

    #[derive(Clone, Copy, Default)]
    struct Rect {
        x: f64,
        y: f64,
        width: f64,
        height: f64,
    }

    #[derive(Clone, Copy, Default)]
    struct Color {
        red: u16,
        green: u16,
        blue: u16,
        alpha: u16,
    }

    #[derive(Clone, Copy, Default)]
    struct PathElement {
        verb: u8,
        x1: f64,
        y1: f64,
        x2: f64,
        y2: f64,
        x3: f64,
        y3: f64,
    }

    #[derive(Clone, Copy, Default)]
    struct Stroke {
        width: f64,
        cap: u8,
        join: u8,
        miter_limit: f64,
    }

    #[derive(Clone, Copy, Default)]
    struct LinePatternStyle {
        color: Color,
        angle: f64,
        spacing: f64,
        offset: f64,
        line_width: f64,
    }

    #[derive(Clone, Copy, Default)]
    struct FrameHeader {
        frame_id: u64,
        revision: u64,
        surface_sequence: u64,
        width: u32,
        height: u32,
        world_to_surface: Transform,
        background: Color,
    }

    #[derive(Clone, Copy, Default)]
    struct FrameResult {
        frame_id: u64,
        revision: u64,
        surface_sequence: u64,
        status: u8,
        scene_count: u32,
    }

    extern "Rust" {
        type SceneBuilder;
        type RetainedScene;
        type RetainedImage;
        type FrameBuilder;
        type Renderer;

        fn begin_scene(revision: u64, world_bounds: Rect, scale_hint: f64) -> Box<SceneBuilder>;
        fn scene_push_transform(scene: &mut SceneBuilder, transform: Transform);
        fn scene_pop_transform(scene: &mut SceneBuilder);
        fn scene_push_clip(scene: &mut SceneBuilder, fill_rule: u8, path: &[PathElement]);
        fn scene_pop_clip(scene: &mut SceneBuilder);
        fn scene_push_layer(scene: &mut SceneBuilder, opacity: f64, blend: u8);
        fn scene_pop_layer(scene: &mut SceneBuilder);
        fn scene_fill_path(
            scene: &mut SceneBuilder,
            fill_rule: u8,
            path: &[PathElement],
            color: Color,
        );
        fn scene_stroke_path(
            scene: &mut SceneBuilder,
            path: &[PathElement],
            color: Color,
            stroke: Stroke,
            dashes: &[f64],
            dash_offset: f64,
        );
        fn scene_fill_ellipse(scene: &mut SceneBuilder, bounds: Rect, color: Color);
        fn scene_stroke_ellipse(
            scene: &mut SceneBuilder,
            bounds: Rect,
            color: Color,
            stroke: Stroke,
        );
        fn new_retained_image(
            image_bytes: &[u8],
            width: u32,
            height: u32,
            stride: u32,
            alpha_type: u8,
        ) -> Box<RetainedImage>;
        fn scene_draw_image(
            scene: &mut SceneBuilder,
            image: &RetainedImage,
            source: Rect,
            image_to_scene: Transform,
            opacity: f64,
        ) -> bool;
        fn scene_draw_line_pattern(
            scene: &mut SceneBuilder,
            fill_rule: u8,
            outline: &[PathElement],
            style: LinePatternStyle,
        );
        fn finish_scene(scene: Box<SceneBuilder>) -> Box<RetainedScene>;
        fn retained_scene_revision(scene: &RetainedScene) -> u64;
        fn retained_scene_command_count(scene: &RetainedScene) -> u64;

        fn new_frame(header: FrameHeader) -> Box<FrameBuilder>;
        fn frame_add_pass(
            frame: &mut FrameBuilder,
            scene: &RetainedScene,
            blend: u8,
            opacity: f64,
            isolated: bool,
            transform: Transform,
        );

        fn new_renderer() -> Box<Renderer>;
        fn renderer_set_surface(renderer: &Renderer, state: SurfaceState) -> bool;
        fn renderer_submit(renderer: &Renderer, frame: Box<FrameBuilder>) -> bool;
        fn renderer_render_offscreen(renderer: &Renderer, frame: Box<FrameBuilder>) -> Vec<u8>;
        fn renderer_try_take_result(renderer: &Renderer) -> FrameResult;
        fn renderer_last_error(renderer: &Renderer) -> String;
    }
}

const SURFACE_UNAVAILABLE: u8 = 0;
const SURFACE_EXPOSED: u8 = 2;

const PLATFORM_APPKIT: u8 = 1;
const PLATFORM_WIN32: u8 = 2;
const PLATFORM_ANDROID_NDK: u8 = 3;
const PLATFORM_WAYLAND: u8 = 4;
const PLATFORM_XCB: u8 = 5;
const PLATFORM_UIKIT: u8 = 6;

const FRAME_NONE: u8 = 0;
const FRAME_PRESENTED: u8 = 1;
const FRAME_TARGET_UNAVAILABLE: u8 = 2;
const FRAME_DROPPED_STALE: u8 = 3;
const FRAME_SURFACE_LOST: u8 = 4;
const FRAME_ERROR: u8 = 5;

/// One bump-safe recording unit: a vello scene plus the scale-independent
/// tallies from which its GPU bump load at any display scale is estimated.
struct SubScene {
    scene: Scene,
    load: LoadTally,
}

pub struct SceneBuilder {
    scenes: Vec<SubScene>,
    scene: Scene,
    load: LoadTally,
    group_depth: usize,
    scale_hint: f64,
    revision: u64,
    command_count: u64,
    transforms: Vec<Affine>,
    world_bounds: Rect,
}

pub struct RetainedScene {
    scenes: Arc<Vec<SubScene>>,
    revision: u64,
    command_count: u64,
}

pub struct RetainedImage {
    image: Option<ImageData>,
}

pub struct FrameBuilder {
    header: ffi::FrameHeader,
    passes: Vec<FramePass>,
}

struct FramePass {
    scenes: Arc<Vec<SubScene>>,
    blend: u8,
    opacity: f32,
    isolated: bool,
    transform: Affine,
}

struct OffscreenRequest {
    frame: Box<FrameBuilder>,
    response: Sender<Result<Vec<u8>, String>>,
}

fn finite_transform(value: ffi::Transform) -> Option<Affine> {
    let values = [
        value.m11, value.m12, value.m21, value.m22, value.dx, value.dy,
    ];
    values
        .iter()
        .all(|component| component.is_finite())
        .then(|| Affine::new(values))
}

fn rect(value: ffi::Rect) -> Option<Rect> {
    if !value.x.is_finite()
        || !value.y.is_finite()
        || !value.width.is_finite()
        || !value.height.is_finite()
        || value.width <= 0.0
        || value.height <= 0.0
    {
        return None;
    }
    Some(Rect::new(
        value.x,
        value.y,
        value.x + value.width,
        value.y + value.height,
    ))
}

fn path(elements: &[ffi::PathElement]) -> Option<BezPath> {
    let mut path = BezPath::new();
    for element in elements {
        let coordinates = [
            element.x1, element.y1, element.x2, element.y2, element.x3, element.y3,
        ];
        if !coordinates.iter().all(|value| value.is_finite()) {
            return None;
        }
        match element.verb {
            0 => path.move_to((element.x1, element.y1)),
            1 => path.line_to((element.x1, element.y1)),
            2 => path.curve_to(
                (element.x1, element.y1),
                (element.x2, element.y2),
                (element.x3, element.y3),
            ),
            3 => path.close_path(),
            _ => return None,
        }
    }
    Some(path)
}

fn fill_rule(value: u8) -> Fill {
    if value == 0 {
        Fill::EvenOdd
    } else {
        Fill::NonZero
    }
}

fn blend_mode(value: u8) -> BlendMode {
    if value == 1 {
        BlendMode::new(Mix::Multiply, Compose::SrcOver)
    } else {
        BlendMode::new(Mix::Normal, Compose::SrcOver)
    }
}

fn color(value: ffi::Color, opacity: f32) -> Color {
    let channel = |component: u16| ((u32::from(component) * 255 + 32767) / 65535) as u8;
    let alpha = (f32::from(value.alpha) * opacity.clamp(0.0, 1.0) / 257.0)
        .round()
        .clamp(0.0, 255.0) as u8;
    Color::from_rgba8(
        channel(value.red),
        channel(value.green),
        channel(value.blue),
        alpha,
    )
}

fn stroke(value: ffi::Stroke) -> Option<Stroke> {
    if !value.width.is_finite()
        || value.width <= 0.0
        || !value.miter_limit.is_finite()
        || value.miter_limit <= 0.0
    {
        return None;
    }
    let mut stroke = Stroke::new(value.width);
    stroke.start_cap = match value.cap {
        1 => Cap::Round,
        2 => Cap::Square,
        _ => Cap::Butt,
    };
    stroke.end_cap = stroke.start_cap;
    stroke.join = match value.join {
        0 => Join::Bevel,
        2 => Join::Round,
        _ => Join::Miter,
    };
    stroke.miter_limit = value.miter_limit;
    Some(stroke)
}

/// The largest per-axis scale factor of an affine map, as an upper bound on
/// how much it stretches lengths.
fn affine_scale(transform: Affine) -> f64 {
    let [m11, m12, m21, m22, ..] = transform.as_coeffs();
    let x = f64::hypot(m11, m12);
    let y = f64::hypot(m21, m22);
    let scale = x.max(y);
    if scale.is_finite() && scale > 0.0 {
        scale
    } else {
        1.0
    }
}

/// Zoom-scalable estimate of the GPU bump work recorded content needs.
///
/// vello flattens curves and counts line-to-tile crossings on the GPU, so
/// both grow with the display scale even though the encoding stays fixed.
/// (vello's own `bump_estimate` feature is unusable here: its `LineTo`
/// crossing estimate measures from the subpath's first point instead of the
/// previous point, which inflates long polylines quadratically.) The tallies
/// are kept in scene units: `lines` counts flattened lines from straight
/// geometry, joins, caps and dash pieces; `curve_subdivisions` scales with
/// the square root of the display scale (Wang-style curve flattening); and
/// `flat_units` is the Manhattan length of drawn geometry, from which tile
/// crossings follow as at most `d / 16 + 2` per line of device Manhattan
/// length `d`.
#[derive(Clone, Copy, Default)]
struct LoadTally {
    lines: f64,
    curve_subdivisions: f64,
    flat_units: f64,
}

/// Estimated bump-buffer element use of content at one display scale.
#[derive(Clone, Copy, Default)]
struct BumpUse {
    lines: u64,
    crossings: u64,
}

impl LoadTally {
    fn add(&mut self, other: LoadTally) {
        self.lines += other.lines;
        self.curve_subdivisions += other.curve_subdivisions;
        self.flat_units += other.flat_units;
    }

    fn estimate(&self, scale: f64) -> BumpUse {
        let scale = if scale.is_finite() && scale > 0.0 {
            scale
        } else {
            1.0
        };
        let lines = self.lines + self.curve_subdivisions * scale.sqrt();
        let crossings = 2.0 * lines + self.flat_units * scale * 0.0625;
        BumpUse {
            lines: lines.min(u64::MAX as f64) as u64,
            crossings: crossings.min(u64::MAX as f64) as u64,
        }
    }
}

/// Per-path geometry totals in scene units (the recording transform is
/// already applied via `scale`).
#[derive(Clone, Copy, Default)]
struct PathMeasure {
    lines: f64,
    curve_subdivisions: f64,
    flat_units: f64,
}

impl PathMeasure {
    fn line(&mut self, from: (f64, f64), to: (f64, f64), scale: f64) {
        let d = ((to.0 - from.0).abs() + (to.1 - from.1).abs()) * scale;
        if d.is_finite() {
            self.lines += 1.0;
            self.flat_units += d;
        }
    }

    fn curve(&mut self, control_manhattan: f64, scale: f64) {
        let d = control_manhattan * scale;
        if d.is_finite() {
            self.curve_subdivisions += 2.0 * d.max(0.0).sqrt();
            self.flat_units += d;
        }
    }
}

fn measure_path(elements: &[ffi::PathElement], scale: f64) -> PathMeasure {
    let mut measure = PathMeasure::default();
    let mut first = (0.0, 0.0);
    let mut last = (0.0, 0.0);
    let mut open = false;
    for element in elements {
        match element.verb {
            0 => {
                if open {
                    measure.line(last, first, scale);
                }
                first = (element.x1, element.y1);
                last = first;
                open = false;
            }
            1 => {
                let to = (element.x1, element.y1);
                measure.line(last, to, scale);
                last = to;
                open = true;
            }
            2 => {
                let p1 = (element.x1, element.y1);
                let p2 = (element.x2, element.y2);
                let p3 = (element.x3, element.y3);
                let control = (p1.0 - last.0).abs()
                    + (p1.1 - last.1).abs()
                    + (p2.0 - p1.0).abs()
                    + (p2.1 - p1.1).abs()
                    + (p3.0 - p2.0).abs()
                    + (p3.1 - p2.1).abs();
                measure.curve(control, scale);
                last = p3;
                open = true;
            }
            3 => {
                measure.line(last, first, scale);
                last = first;
                open = false;
            }
            _ => {}
        }
    }
    if open {
        // Fills close open subpaths implicitly.
        measure.line(last, first, scale);
    }
    measure
}

impl SceneBuilder {
    fn transform(&self) -> Affine {
        self.transforms.last().copied().unwrap_or(Affine::IDENTITY)
    }

    fn viewport(&self) -> Rect {
        self.world_bounds
    }

    /// The measurement scale for the current recording transform.
    fn measure_scale(&self) -> f64 {
        affine_scale(self.transform())
    }

    /// Seals the current sub-scene once it reaches the bump-safe encoding
    /// budget — by encoded segment count, or by estimated bump-buffer load
    /// at the scale this scene was recorded for (flattened lines and tile
    /// crossings grow with zoom, so high-zoom recordings must split much
    /// finer than the encoded segment count alone suggests). The record
    /// share is far below the compose-time budget to leave headroom for the
    /// device pixel ratio, for transient camera zoom beyond the recorded
    /// scale, and for estimate error. Splitting is only sound between
    /// top-level draws: an open clip or blend group must stay within one
    /// sub-scene.
    fn split_before_append(&mut self) {
        if self.group_depth != 0 {
            return;
        }
        let seal = u64::from(self.scene.encoding().n_path_segments) >= batch_segment_budget() || {
            let bump = self.load.estimate(self.scale_hint);
            bump.lines >= BUMP_LINES_CAPACITY / 8 || bump.crossings >= BUMP_SEGMENTS_CAPACITY / 8
        };
        if seal {
            self.scenes.push(SubScene {
                scene: std::mem::replace(&mut self.scene, Scene::new()),
                load: std::mem::take(&mut self.load),
            });
        }
    }
}

fn begin_scene(revision: u64, world_bounds: ffi::Rect, scale_hint: f64) -> Box<SceneBuilder> {
    Box::new(SceneBuilder {
        scenes: Vec::new(),
        scene: Scene::new(),
        load: LoadTally::default(),
        group_depth: 0,
        scale_hint: if scale_hint.is_finite() && scale_hint > 0.0 {
            scale_hint
        } else {
            1.0
        },
        revision,
        command_count: 0,
        transforms: vec![Affine::IDENTITY],
        world_bounds: rect(world_bounds).unwrap_or(Rect::new(-1.0e9, -1.0e9, 1.0e9, 1.0e9)),
    })
}

fn scene_push_transform(scene: &mut SceneBuilder, transform: ffi::Transform) {
    scene.command_count += 1;
    if let Some(transform) = finite_transform(transform) {
        scene.transforms.push(scene.transform() * transform);
    } else {
        scene.transforms.push(scene.transform());
    }
}

fn scene_pop_transform(scene: &mut SceneBuilder) {
    scene.command_count += 1;
    if scene.transforms.len() > 1 {
        scene.transforms.pop();
    }
}

fn scene_push_clip(scene: &mut SceneBuilder, rule: u8, elements: &[ffi::PathElement]) {
    scene.command_count += 1;
    scene.split_before_append();
    scene.group_depth += 1;
    if let Some(path) = path(elements) {
        let measure = measure_path(elements, scene.measure_scale());
        scene.load.add(LoadTally {
            lines: measure.lines,
            curve_subdivisions: measure.curve_subdivisions,
            flat_units: measure.flat_units,
        });
        scene
            .scene
            .push_clip_layer(fill_rule(rule), scene.transform(), &path);
    } else {
        let empty = BezPath::new();
        scene
            .scene
            .push_clip_layer(Fill::NonZero, Affine::IDENTITY, &empty);
    }
}

fn scene_pop_clip(scene: &mut SceneBuilder) {
    scene.command_count += 1;
    scene.group_depth = scene.group_depth.saturating_sub(1);
    scene.scene.pop_layer();
}

fn scene_push_layer(scene: &mut SceneBuilder, opacity: f64, blend: u8) {
    scene.command_count += 1;
    scene.split_before_append();
    scene.group_depth += 1;
    scene.load.add(LoadTally {
        lines: 4.0,
        ..LoadTally::default()
    });
    let clip = scene.viewport();
    scene.scene.push_layer(
        Fill::NonZero,
        blend_mode(blend),
        opacity.clamp(0.0, 1.0) as f32,
        Affine::IDENTITY,
        &clip,
    );
}

fn scene_pop_layer(scene: &mut SceneBuilder) {
    scene.command_count += 1;
    scene.group_depth = scene.group_depth.saturating_sub(1);
    scene.scene.pop_layer();
}

fn scene_fill_path(
    scene: &mut SceneBuilder,
    rule: u8,
    elements: &[ffi::PathElement],
    brush: ffi::Color,
) {
    scene.command_count += 1;
    scene.split_before_append();
    if let Some(path) = path(elements) {
        let measure = measure_path(elements, scene.measure_scale());
        scene.load.add(LoadTally {
            lines: measure.lines,
            curve_subdivisions: measure.curve_subdivisions,
            flat_units: measure.flat_units,
        });
        scene.scene.fill(
            fill_rule(rule),
            scene.transform(),
            color(brush, 1.0),
            None,
            &path,
        );
    }
}

fn scene_stroke_path(
    scene: &mut SceneBuilder,
    elements: &[ffi::PathElement],
    brush: ffi::Color,
    style: ffi::Stroke,
    dashes: &[f64],
    dash_offset: f64,
) {
    scene.command_count += 1;
    scene.split_before_append();
    let valid_dashes =
        dash_offset.is_finite() && dashes.iter().all(|value| value.is_finite() && *value > 0.0);
    if let (Some(path), Some(mut stroke)) = (path(elements), stroke(style))
        && valid_dashes
    {
        let scale = scene.measure_scale();
        let measure = measure_path(elements, scale);
        let width = style.width * scale;
        // Both offset sides plus join and cap fans; dashing splits the
        // stroke into capped pieces whose count follows the pattern period
        // in path units.
        let dash_period: f64 = dashes.iter().sum();
        let dash_pieces = if dash_period > 0.0 && scale > 0.0 {
            measure.flat_units / (dash_period * scale)
        } else {
            0.0
        };
        scene.load.add(LoadTally {
            lines: 4.0 * measure.lines + 4.0 * dash_pieces,
            curve_subdivisions: 2.0 * measure.curve_subdivisions,
            flat_units: 2.0 * measure.flat_units
                + (3.0 * measure.lines + 2.0 * dash_pieces) * width,
        });
        stroke.dash_pattern.extend_from_slice(dashes);
        stroke.dash_offset = dash_offset;
        scene
            .scene
            .stroke(&stroke, scene.transform(), color(brush, 1.0), None, &path);
    }
}

fn scene_fill_ellipse(scene: &mut SceneBuilder, bounds: ffi::Rect, brush: ffi::Color) {
    scene.command_count += 1;
    scene.split_before_append();
    if let Some(bounds) = rect(bounds) {
        let perimeter = 2.0 * (bounds.width() + bounds.height()) * scene.measure_scale();
        scene.load.add(LoadTally {
            lines: 4.0,
            curve_subdivisions: 4.0 * perimeter.max(0.0).sqrt(),
            flat_units: perimeter,
        });
        scene.scene.fill(
            Fill::NonZero,
            scene.transform(),
            color(brush, 1.0),
            None,
            &Ellipse::from_rect(bounds),
        );
    }
}

fn scene_stroke_ellipse(
    scene: &mut SceneBuilder,
    bounds: ffi::Rect,
    brush: ffi::Color,
    style: ffi::Stroke,
) {
    scene.command_count += 1;
    scene.split_before_append();
    if let (Some(bounds), Some(stroke)) = (rect(bounds), stroke(style)) {
        let scale = scene.measure_scale();
        let perimeter = 2.0 * (bounds.width() + bounds.height()) * scale;
        scene.load.add(LoadTally {
            lines: 8.0,
            curve_subdivisions: 8.0 * perimeter.max(0.0).sqrt(),
            flat_units: 2.0 * perimeter + 4.0 * style.width * scale,
        });
        scene.scene.stroke(
            &stroke,
            scene.transform(),
            color(brush, 1.0),
            None,
            &Ellipse::from_rect(bounds),
        );
    }
}

fn new_retained_image(
    image_bytes: &[u8],
    width: u32,
    height: u32,
    stride: u32,
    alpha_type: u8,
) -> Box<RetainedImage> {
    let Some(row_bytes) = width.checked_mul(4) else {
        return Box::new(RetainedImage { image: None });
    };
    let Some(required) = usize::try_from(stride)
        .ok()
        .and_then(|stride| stride.checked_mul(usize::try_from(height).ok()?))
    else {
        return Box::new(RetainedImage { image: None });
    };
    if width == 0 || height == 0 || stride < row_bytes || required > image_bytes.len() {
        return Box::new(RetainedImage { image: None });
    }

    let tight_length = usize::try_from(row_bytes)
        .ok()
        .and_then(|row| row.checked_mul(usize::try_from(height).ok()?));
    let Some(tight_length) = tight_length else {
        return Box::new(RetainedImage { image: None });
    };
    let mut pixels = Vec::with_capacity(tight_length);
    let stride = stride as usize;
    let row_bytes = row_bytes as usize;
    for row in 0..height as usize {
        let start = row * stride;
        pixels.extend_from_slice(&image_bytes[start..start + row_bytes]);
    }

    Box::new(RetainedImage {
        image: Some(ImageData {
            data: Blob::from(pixels),
            format: ImageFormat::Rgba8,
            alpha_type: if alpha_type == 1 {
                ImageAlphaType::AlphaPremultiplied
            } else {
                ImageAlphaType::Alpha
            },
            width,
            height,
        }),
    })
}

fn scene_draw_image(
    scene: &mut SceneBuilder,
    retained: &RetainedImage,
    source: ffi::Rect,
    image_to_scene: ffi::Transform,
    opacity: f64,
) -> bool {
    scene.command_count += 1;
    scene.split_before_append();
    let Some(source) = rect(source) else {
        return false;
    };
    let Some(image_to_scene) = finite_transform(image_to_scene) else {
        return false;
    };
    let Some(image) = retained.image.clone() else {
        return false;
    };
    if !opacity.is_finite() {
        return false;
    }
    let width = image.width;
    let height = image.height;
    let source = Rect::new(
        source.x0.max(0.0),
        source.y0.max(0.0),
        source.x1.min(f64::from(width)),
        source.y1.min(f64::from(height)),
    );
    if source.width() <= 0.0 || source.height() <= 0.0 {
        return false;
    }
    let image_scale = affine_scale(scene.transform() * image_to_scene);
    scene.load.add(LoadTally {
        lines: 4.0,
        curve_subdivisions: 0.0,
        flat_units: 2.0 * (source.width() + source.height()) * image_scale,
    });
    let brush = ImageBrush {
        image,
        sampler: ImageSampler {
            x_extend: Extend::Pad,
            y_extend: Extend::Pad,
            quality: ImageQuality::Medium,
            alpha: opacity.clamp(0.0, 1.0) as f32,
        },
    };
    scene.scene.fill(
        Fill::NonZero,
        scene.transform() * image_to_scene,
        &brush,
        None,
        &source,
    );
    true
}

fn scene_draw_line_pattern(
    scene: &mut SceneBuilder,
    rule: u8,
    outline: &[ffi::PathElement],
    style: ffi::LinePatternStyle,
) {
    scene.command_count += 1;
    scene.split_before_append();
    let Some(path) = path(outline) else {
        return;
    };
    if !style.angle.is_finite()
        || !style.spacing.is_finite()
        || !style.offset.is_finite()
        || !style.line_width.is_finite()
        || style.spacing <= 0.0
        || style.line_width <= 0.0
    {
        return;
    }
    let measure = measure_path(outline, scene.measure_scale());
    scene.load.add(LoadTally {
        lines: measure.lines,
        curve_subdivisions: measure.curve_subdivisions,
        flat_units: measure.flat_units,
    });
    if style.line_width >= style.spacing {
        scene.scene.fill(
            fill_rule(rule),
            scene.transform(),
            color(style.color, 1.0),
            None,
            &path,
        );
        return;
    }

    let coverage = (style.line_width / style.spacing).clamp(0.0, 1.0) as f32;
    let solid = color(style.color, 1.0);
    let clear = color(
        ffi::Color {
            alpha: 0,
            ..style.color
        },
        1.0,
    );
    let half = coverage * 0.5;
    let stops: [ColorStop; 6] = [
        (0.0, solid).into(),
        (half, solid).into(),
        (half, clear).into(),
        (1.0 - half, clear).into(),
        (1.0 - half, solid).into(),
        (1.0, solid).into(),
    ];
    let gradient = Gradient::new_linear((0.0, 0.0), (style.spacing, 0.0))
        .with_extend(Extend::Repeat)
        .with_stops(stops);
    let (sin_angle, cos_angle) = style.angle.sin_cos();
    let pattern_transform = Affine::new([
        sin_angle,
        cos_angle,
        cos_angle,
        -sin_angle,
        sin_angle * style.offset,
        cos_angle * style.offset,
    ]);
    scene.scene.fill(
        fill_rule(rule),
        scene.transform(),
        &gradient,
        Some(pattern_transform),
        &path,
    );
}

fn finish_scene(scene: Box<SceneBuilder>) -> Box<RetainedScene> {
    let mut scenes = scene.scenes;
    scenes.push(SubScene {
        scene: scene.scene,
        load: scene.load,
    });
    Box::new(RetainedScene {
        scenes: Arc::new(scenes),
        revision: scene.revision,
        command_count: scene.command_count,
    })
}

fn retained_scene_revision(scene: &RetainedScene) -> u64 {
    scene.revision
}

fn retained_scene_command_count(scene: &RetainedScene) -> u64 {
    scene.command_count
}

fn new_frame(header: ffi::FrameHeader) -> Box<FrameBuilder> {
    Box::new(FrameBuilder {
        header,
        passes: Vec::new(),
    })
}

fn frame_add_pass(
    frame: &mut FrameBuilder,
    scene: &RetainedScene,
    blend: u8,
    opacity: f64,
    isolated: bool,
    transform: ffi::Transform,
) {
    let Some(transform) = finite_transform(transform) else {
        return;
    };
    if !opacity.is_finite() || opacity <= 0.0 {
        return;
    }
    frame.passes.push(FramePass {
        scenes: Arc::clone(&scene.scenes),
        blend,
        opacity: opacity.clamp(0.0, 1.0) as f32,
        isolated,
        transform,
    });
}

struct PreparedSurface {
    instance: wgpu::Instance,
    surface: wgpu::Surface<'static>,
}

struct SurfaceUpdate {
    state: ffi::SurfaceState,
    prepared: Option<Result<PreparedSurface, String>>,
}

enum Control {
    SetSurface(SurfaceUpdate),
    Shutdown,
}

struct RenderTexture {
    _texture: wgpu::Texture,
    view: wgpu::TextureView,
    width: u32,
    height: u32,
}

struct SurfaceTarget {
    _instance: wgpu::Instance,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    renderer: VelloRenderer,
    blitter: TextureBlitter,
    blend_blitter: TextureBlitter,
    compositor: GroupCompositor,
    config: wgpu::SurfaceConfiguration,
    render_texture: Option<RenderTexture>,
    platform: u8,
    window: u64,
    display: u64,
}

struct OffscreenTarget {
    _instance: wgpu::Instance,
    device: wgpu::Device,
    queue: wgpu::Queue,
    renderer: VelloRenderer,
    blitter: TextureBlitter,
    blend_blitter: TextureBlitter,
    compositor: GroupCompositor,
    resources: Option<OffscreenResources>,
}

const GROUP_COMPOSITE_SHADER: &str = "\
@group(0) @binding(0) var content: texture_2d<f32>;
@group(0) @binding(1) var<uniform> opacity: vec4<f32>;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let x = f32(i32(index & 1u) * 4 - 1);
    let y = f32(i32(index >> 1u) * 4 - 1);
    return vec4<f32>(x, y, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    return textureLoad(content, vec2<i32>(position.xy), 0) * opacity.x;
}
";

struct GroupTexture {
    _texture: wgpu::Texture,
    view: wgpu::TextureView,
    bind_group: wgpu::BindGroup,
    width: u32,
    height: u32,
}

/// Applies a split pass's blend mode and opacity while compositing its
/// accumulated intermediate texture onto the frame.
///
/// A pass recorded as a single sub-scene applies blend and opacity in a
/// vello layer. Once recording splits a pass, that layer cannot span the
/// resulting submissions, so the group's content accumulates source-over in
/// an intermediate texture and this compositor applies the pass blend and
/// opacity in the fixed-function blend units instead.
///
/// Source-over composites with premultiplied-alpha blending after the
/// fragment shader scales the sample by the group opacity. Multiply follows
/// the isolated-group formula `Co = Cs·Cb + Cs·(1−αb) + Cb·(1−αs)` in two
/// fixed-function passes over the same source: the first contributes
/// `Cs·Cb + Cb·(1−αs)` while leaving the backdrop alpha in place, and the
/// second adds `Cs·(1−αb)` and resolves the union alpha
/// `αs + αb − αs·αb`.
struct GroupCompositor {
    format: wgpu::TextureFormat,
    layout: wgpu::BindGroupLayout,
    source_over: wgpu::RenderPipeline,
    multiply_backdrop: wgpu::RenderPipeline,
    multiply_source: wgpu::RenderPipeline,
    opacity: wgpu::Buffer,
    target: Option<GroupTexture>,
}

impl GroupCompositor {
    fn new(device: &wgpu::Device, format: wgpu::TextureFormat) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("Mapper Vello group composite"),
            source: wgpu::ShaderSource::Wgsl(GROUP_COMPOSITE_SHADER.into()),
        });
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("Mapper Vello group composite"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(16),
                    },
                    count: None,
                },
            ],
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("Mapper Vello group composite"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = |label: &'static str, blend: wgpu::BlendState| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    compilation_options: wgpu::PipelineCompilationOptions::default(),
                    buffers: &[],
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_main"),
                    compilation_options: wgpu::PipelineCompilationOptions::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend: Some(blend),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let source_over = pipeline(
            "Mapper Vello group source-over",
            wgpu::BlendState::PREMULTIPLIED_ALPHA_BLENDING,
        );
        let backdrop_blend = wgpu::BlendComponent {
            src_factor: wgpu::BlendFactor::Dst,
            dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
            operation: wgpu::BlendOperation::Add,
        };
        let multiply_backdrop = pipeline(
            "Mapper Vello group multiply backdrop",
            wgpu::BlendState {
                color: backdrop_blend,
                alpha: backdrop_blend,
            },
        );
        let source_blend = wgpu::BlendComponent {
            src_factor: wgpu::BlendFactor::OneMinusDstAlpha,
            dst_factor: wgpu::BlendFactor::One,
            operation: wgpu::BlendOperation::Add,
        };
        let multiply_source = pipeline(
            "Mapper Vello group multiply source",
            wgpu::BlendState {
                color: source_blend,
                alpha: source_blend,
            },
        );
        let opacity = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("Mapper Vello group opacity"),
            size: 16,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        Self {
            format,
            layout,
            source_over,
            multiply_backdrop,
            multiply_source,
            opacity,
            target: None,
        }
    }

    fn target_view(&mut self, device: &wgpu::Device, width: u32, height: u32) -> wgpu::TextureView {
        let recreate = self
            .target
            .as_ref()
            .is_none_or(|target| target.width != width || target.height != height);
        if recreate {
            let texture = device.create_texture(&wgpu::TextureDescriptor {
                label: Some("Mapper Vello group target"),
                size: wgpu::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: self.format,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
            let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("Mapper Vello group composite"),
                layout: &self.layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(&view),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: self.opacity.as_entire_binding(),
                    },
                ],
            });
            self.target = Some(GroupTexture {
                _texture: texture,
                view,
                bind_group,
                width,
                height,
            });
        }
        self.target
            .as_ref()
            .expect("group target exists")
            .view
            .clone()
    }

    fn composite(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        output: &wgpu::TextureView,
        blend: u8,
        opacity: f32,
    ) {
        let target = self.target.as_ref().expect("group target exists");
        let mut opacity_data = [0u8; 16];
        opacity_data[..4].copy_from_slice(&opacity.clamp(0.0, 1.0).to_le_bytes());
        queue.write_buffer(&self.opacity, 0, &opacity_data);
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("Mapper Vello group composite"),
        });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("Mapper Vello group composite"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: output,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Load,
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            let pipelines: &[&wgpu::RenderPipeline] = if blend == 1 {
                &[&self.multiply_backdrop, &self.multiply_source]
            } else {
                &[&self.source_over]
            };
            for pipeline in pipelines {
                pass.set_pipeline(pipeline);
                pass.set_bind_group(0, &target.bind_group, &[]);
                pass.draw(0..3, 0..1);
            }
        }
        queue.submit(Some(encoder.finish()));
    }
}

struct OffscreenResources {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    _render_texture: wgpu::Texture,
    render_view: wgpu::TextureView,
    buffer: wgpu::Buffer,
    width: u32,
    height: u32,
    unpadded_bytes_per_row: u32,
    padded_bytes_per_row: u32,
}

pub struct Renderer {
    control_tx: Sender<Control>,
    frame_tx: Sender<Box<FrameBuilder>>,
    frame_replace_rx: Receiver<Box<FrameBuilder>>,
    offscreen_tx: Sender<OffscreenRequest>,
    result_rx: Receiver<ffi::FrameResult>,
    last_error: Arc<Mutex<String>>,
    prepared_descriptor: Arc<Mutex<Option<(u8, u64, u64)>>>,
    thread: Mutex<Option<thread::JoinHandle<()>>>,
}

fn raw_handle(value: u64) -> Option<std::ptr::NonNull<std::ffi::c_void>> {
    std::ptr::NonNull::new(value as *mut std::ffi::c_void)
}

fn surface_handles(
    state: ffi::SurfaceState,
) -> Result<(Option<RawDisplayHandle>, RawWindowHandle), String> {
    match state.platform {
        PLATFORM_APPKIT => {
            let view = raw_handle(state.window).ok_or("AppKit surface has no NSView")?;
            Ok((
                Some(AppKitDisplayHandle::new().into()),
                AppKitWindowHandle::new(view).into(),
            ))
        }
        PLATFORM_WIN32 => {
            let hwnd =
                NonZeroIsize::new(state.window as isize).ok_or("Win32 surface has no HWND")?;
            Ok((
                Some(WindowsDisplayHandle::new().into()),
                Win32WindowHandle::new(hwnd).into(),
            ))
        }
        PLATFORM_ANDROID_NDK => {
            let window = raw_handle(state.window).ok_or("Android surface has no ANativeWindow")?;
            Ok((
                Some(AndroidDisplayHandle::new().into()),
                AndroidNdkWindowHandle::new(window).into(),
            ))
        }
        PLATFORM_UIKIT => {
            let view = raw_handle(state.window).ok_or("UIKit surface has no UIView")?;
            Ok((
                Some(UiKitDisplayHandle::new().into()),
                UiKitWindowHandle::new(view).into(),
            ))
        }
        PLATFORM_WAYLAND => {
            let display = raw_handle(state.display).ok_or("Wayland surface has no wl_display")?;
            let surface = raw_handle(state.window).ok_or("Wayland surface has no wl_surface")?;
            Ok((
                Some(WaylandDisplayHandle::new(display).into()),
                WaylandWindowHandle::new(surface).into(),
            ))
        }
        PLATFORM_XCB => {
            let connection = raw_handle(state.display);
            let window =
                NonZeroU32::new(state.window as u32).ok_or("XCB surface has no xcb_window_t")?;
            Ok((
                Some(XcbDisplayHandle::new(connection, 0).into()),
                XcbWindowHandle::new(window).into(),
            ))
        }
        _ => Err("unsupported Qt native surface platform".to_owned()),
    }
}

fn platform_backends(platform: u8) -> wgpu::Backends {
    match platform {
        PLATFORM_APPKIT | PLATFORM_UIKIT => wgpu::Backends::METAL,
        PLATFORM_WIN32 => wgpu::Backends::DX12,
        PLATFORM_ANDROID_NDK | PLATFORM_WAYLAND | PLATFORM_XCB => wgpu::Backends::VULKAN,
        _ => wgpu::Backends::PRIMARY,
    }
}

fn instance_descriptor(platform_defaults: wgpu::Backends) -> wgpu::InstanceDescriptor {
    let mut descriptor = wgpu::InstanceDescriptor::new_without_display_handle_from_env();
    descriptor.backends &= platform_defaults;
    descriptor
}

async fn request_adapter<'a>(
    instance: &wgpu::Instance,
    compatible_surface: Option<&'a wgpu::Surface<'a>>,
) -> Result<wgpu::Adapter, wgpu::RequestAdapterError> {
    if std::env::var_os("WGPU_ADAPTER_NAME").is_some() {
        return wgpu::util::initialize_adapter_from_env(instance, compatible_surface).await;
    }

    instance
        .request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            force_fallback_adapter: false,
            compatible_surface,
        })
        .await
}

fn device_descriptor(adapter: &wgpu::Adapter) -> wgpu::DeviceDescriptor<'static> {
    let adapter_limits = adapter.limits();
    let mut required_limits = wgpu::Limits::default();
    required_limits.max_inter_stage_shader_variables = required_limits
        .max_inter_stage_shader_variables
        .min(adapter_limits.max_inter_stage_shader_variables);
    wgpu::DeviceDescriptor {
        label: Some("Mapper Vello device"),
        required_features: wgpu::Features::empty(),
        required_limits,
        experimental_features: wgpu::ExperimentalFeatures::disabled(),
        memory_hints: wgpu::MemoryHints::Performance,
        trace: wgpu::Trace::Off,
    }
}

fn presentation_format(formats: &[wgpu::TextureFormat]) -> Option<wgpu::TextureFormat> {
    formats.iter().copied().find(|format| {
        matches!(
            format,
            wgpu::TextureFormat::Rgba8Unorm | wgpu::TextureFormat::Bgra8Unorm
        )
    })
}

fn prepare_surface(state: ffi::SurfaceState) -> Result<PreparedSurface, String> {
    if state.phase != SURFACE_EXPOSED || state.width == 0 || state.height == 0 {
        return Err("surface is not exposed at a nonzero size".to_owned());
    }
    let (raw_display_handle, raw_window_handle) = surface_handles(state)?;
    let instance = wgpu::Instance::new(instance_descriptor(platform_backends(state.platform)));
    let surface = unsafe {
        instance
            .create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
                raw_display_handle,
                raw_window_handle,
            })
            .map_err(|error| format!("wgpu surface creation failed: {error}"))?
    };
    Ok(PreparedSurface { instance, surface })
}

fn create_surface_target(
    state: ffi::SurfaceState,
    prepared: PreparedSurface,
) -> Result<SurfaceTarget, String> {
    let PreparedSurface { instance, surface } = prepared;
    let adapter = pollster::block_on(request_adapter(&instance, Some(&surface)))
        .map_err(|error| format!("no compatible wgpu adapter: {error}"))?;
    let adapter_info = adapter.get_info();
    let (device, queue) = pollster::block_on(adapter.request_device(&device_descriptor(&adapter)))
        .map_err(|error| {
            format!(
                "wgpu device creation failed on {} ({:?}, {:?}): {error}",
                adapter_info.name, adapter_info.backend, adapter_info.device_type
            )
        })?;
    let capabilities = surface.get_capabilities(&adapter);
    let format = presentation_format(&capabilities.formats)
        .ok_or("wgpu surface has no supported non-sRGB 8-bit presentation format")?;
    let mut config = surface
        .get_default_config(&adapter, state.width, state.height)
        .ok_or("wgpu surface has no supported default configuration")?;
    config.usage = wgpu::TextureUsages::RENDER_ATTACHMENT;
    config.format = format;
    if capabilities
        .present_modes
        .contains(&wgpu::PresentMode::Fifo)
    {
        config.present_mode = wgpu::PresentMode::Fifo;
    }
    config.alpha_mode = capabilities
        .alpha_modes
        .iter()
        .copied()
        .find(|mode| *mode == wgpu::CompositeAlphaMode::Opaque)
        .unwrap_or(wgpu::CompositeAlphaMode::Auto);
    config.desired_maximum_frame_latency = 2;
    surface.configure(&device, &config);
    let renderer = VelloRenderer::new(
        &device,
        RendererOptions {
            antialiasing_support: AaSupport::area_only(),
            ..RendererOptions::default()
        },
    )
    .map_err(|error| format!("Vello renderer creation failed: {error}"))?;
    let blitter = TextureBlitter::new(&device, config.format);
    // vello's fine stage stores straight (unpremultiplied) alpha, so batch
    // accumulation must blend with straight-alpha source factors; the result
    // accumulates as premultiplied color.
    let blend_blitter = wgpu::util::TextureBlitterBuilder::new(&device, config.format)
        .blend_state(wgpu::BlendState::ALPHA_BLENDING)
        .build();
    let compositor = GroupCompositor::new(&device, config.format);
    Ok(SurfaceTarget {
        _instance: instance,
        surface,
        device,
        queue,
        renderer,
        blitter,
        blend_blitter,
        compositor,
        config,
        render_texture: None,
        platform: state.platform,
        window: state.window,
        display: state.display,
    })
}

fn create_offscreen_target() -> Result<OffscreenTarget, String> {
    let instance = wgpu::Instance::new(instance_descriptor(wgpu::Backends::PRIMARY));
    let adapter = pollster::block_on(request_adapter(&instance, None))
        .map_err(|error| format!("no headless wgpu adapter: {error}"))?;
    let adapter_info = adapter.get_info();
    let (device, queue) = pollster::block_on(adapter.request_device(&device_descriptor(&adapter)))
        .map_err(|error| {
            format!(
                "headless wgpu device creation failed on {} ({:?}, {:?}): {error}",
                adapter_info.name, adapter_info.backend, adapter_info.device_type
            )
        })?;
    let renderer = VelloRenderer::new(
        &device,
        RendererOptions {
            antialiasing_support: AaSupport::area_only(),
            ..RendererOptions::default()
        },
    )
    .map_err(|error| format!("headless Vello renderer creation failed: {error}"))?;
    let blitter = TextureBlitter::new(&device, wgpu::TextureFormat::Rgba8Unorm);
    // Straight-alpha source blending; see create_surface_target.
    let blend_blitter =
        wgpu::util::TextureBlitterBuilder::new(&device, wgpu::TextureFormat::Rgba8Unorm)
            .blend_state(wgpu::BlendState::ALPHA_BLENDING)
            .build();
    let compositor = GroupCompositor::new(&device, wgpu::TextureFormat::Rgba8Unorm);
    Ok(OffscreenTarget {
        _instance: instance,
        device,
        queue,
        renderer,
        blitter,
        blend_blitter,
        compositor,
        resources: None,
    })
}

impl SurfaceTarget {
    fn matches(&self, state: ffi::SurfaceState) -> bool {
        self.platform == state.platform
            && self.window == state.window
            && self.display == state.display
    }

    fn resize(&mut self, width: u32, height: u32) {
        let width = width.max(1);
        let height = height.max(1);
        if self.config.width == width && self.config.height == height {
            return;
        }
        self.config.width = width;
        self.config.height = height;
        self.surface.configure(&self.device, &self.config);
        self.render_texture = None;
    }

    fn render_texture(&mut self, width: u32, height: u32) -> &wgpu::TextureView {
        let recreate = self
            .render_texture
            .as_ref()
            .map(|texture| texture.width != width || texture.height != height)
            .unwrap_or(true);
        if recreate {
            let texture = self.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("Mapper Vello frame target"),
                size: wgpu::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
            self.render_texture = Some(RenderTexture {
                _texture: texture,
                view,
                width,
                height,
            });
        }
        &self
            .render_texture
            .as_ref()
            .expect("frame target exists")
            .view
    }
}

fn publish_result(
    tx: &Sender<ffi::FrameResult>,
    replace_rx: &Receiver<ffi::FrameResult>,
    result: ffi::FrameResult,
) {
    match tx.try_send(result) {
        Ok(()) => {}
        Err(TrySendError::Full(result)) => {
            let _ = replace_rx.try_recv();
            let _ = tx.try_send(result);
        }
        Err(TrySendError::Disconnected(_)) => {}
    }
}

fn set_error(last_error: &Mutex<String>, message: impl Into<String>) {
    if let Ok(mut error) = last_error.lock() {
        *error = message.into();
    }
}

fn frame_result(frame: &FrameBuilder, status: u8) -> ffi::FrameResult {
    ffi::FrameResult {
        frame_id: frame.header.frame_id,
        revision: frame.header.revision,
        surface_sequence: frame.header.surface_sequence,
        status,
        scene_count: frame.passes.len().min(u32::MAX as usize) as u32,
    }
}

/// vello 0.9 sizes several GPU bump buffers with fixed capacities that were
/// "hand picked to accommodate the vello test scenes" (vello_encoding
/// config.rs). A whole-basemap frame overflows them, and the overflow is
/// silent: the affected stages bail and the output stays blank while the
/// render still reports success. Recording splits every retained scene into
/// sub-scenes below this encoded segment count, and rendering combines them
/// into batches bounded by it, so no single vello submission approaches
/// those capacities. Batches render over a transparent base and are
/// composited onto the target with straight-alpha source-over blending
/// (vello stores unpremultiplied output), which is exactly how the passes
/// would combine inside one scene.
const BATCH_ENCODED_SEGMENT_BUDGET: u64 = 250_000;

/// The active budget: the compiled default, or the `MAPPER_VELLO_SEGMENT_BUDGET`
/// environment override for diagnosing suspected bump overflows and for
/// forcing the split machinery onto small scenes in tests.
fn batch_segment_budget() -> u64 {
    use std::sync::OnceLock;
    static BUDGET: OnceLock<u64> = OnceLock::new();
    *BUDGET.get_or_init(|| {
        std::env::var("MAPPER_VELLO_SEGMENT_BUDGET")
            .ok()
            .and_then(|value| value.parse().ok())
            .filter(|value| *value > 0)
            .unwrap_or(BATCH_ENCODED_SEGMENT_BUDGET)
    })
}

/// One bump-safe unit of frame rendering.
///
/// A `Batch` is submitted to vello directly and composited onto the frame
/// with source-over. A `Group` is one pass which must composite against the
/// whole accumulated frame: every pass with a non-source-over blend mode
/// (inside a batch it would only see that batch's content as its backdrop),
/// and every pass whose recording was split across several sub-scenes while
/// its opacity must apply to the content as a whole. The group's batches
/// accumulate in an intermediate texture first, and the pass blend and
/// opacity apply when that texture is composited onto the frame.
enum RenderStep {
    Batch(Box<Scene>),
    Group {
        batches: Vec<Scene>,
        blend: u8,
        opacity: f32,
    },
}

/// Fixed bump-buffer capacities from vello_encoding config.rs
/// (`BufferSizes::new`, "hand picked to accommodate the vello test scenes").
const BUMP_LINES_CAPACITY: u64 = 1 << 21;
const BUMP_SEGMENTS_CAPACITY: u64 = 1 << 21;

/// Bump-buffer load of one prospective vello submission.
///
/// The encoded segment count is transform-independent, but the flattened
/// line count and especially the line-to-tile-crossing count grow with the
/// display scale: zooming far into a map overflows the fixed capacities even
/// though the encoded segment count per submission stays modest.
#[derive(Clone, Copy, Default)]
struct BatchLoad {
    encoded_segments: u64,
    lines: u64,
    crossings: u64,
}

impl BatchLoad {
    fn of(sub_scene: &SubScene, transform: Affine) -> Self {
        let bump = sub_scene.load.estimate(affine_scale(transform));
        Self {
            encoded_segments: u64::from(sub_scene.scene.encoding().n_path_segments),
            lines: bump.lines,
            crossings: bump.crossings,
        }
    }

    fn add(&mut self, other: Self) {
        self.encoded_segments += other.encoded_segments;
        self.lines += other.lines;
        self.crossings += other.crossings;
    }

    /// Whether adding `other` would push one submission beyond the safe
    /// share of any bump buffer.
    fn saturated_by(&self, other: Self) -> bool {
        self.encoded_segments + other.encoded_segments > batch_segment_budget()
            || self.lines + other.lines > BUMP_LINES_CAPACITY / 2
            || self.crossings + other.crossings > BUMP_SEGMENTS_CAPACITY / 2
    }

    /// Whether this load alone risks the silent overflow. Only an
    /// indivisible unit — a single draw or one clip/blend group — reaches
    /// this despite the record-time split.
    fn over_capacity(&self) -> bool {
        self.encoded_segments > 2 * batch_segment_budget()
            || self.lines > BUMP_LINES_CAPACITY
            || self.crossings > BUMP_SEGMENTS_CAPACITY
    }
}

fn warn_over_capacity_scene(load: BatchLoad) {
    use std::sync::atomic::{AtomicU64, Ordering};
    static MAX_WARNED: AtomicU64 = AtomicU64::new(0);
    let worst = load.encoded_segments.max(load.lines).max(load.crossings);
    if worst > MAX_WARNED.fetch_max(worst, Ordering::Relaxed) {
        eprintln!(
            "Mapper Vello: an indivisible scene unit ({} encoded path segments, \
             estimated {} lines and {} tile crossings at the current zoom) exceeds \
             a fixed GPU bump capacity; its render may be silently incomplete",
            load.encoded_segments, load.lines, load.crossings,
        );
    }
}

fn compose_steps(frame: &FrameBuilder) -> Result<Vec<RenderStep>, &'static str> {
    finite_transform(frame.header.world_to_surface).ok_or("frame camera is not finite")?;
    let viewport = Rect::new(
        0.0,
        0.0,
        f64::from(frame.header.width),
        f64::from(frame.header.height),
    );
    let mut steps = Vec::new();
    let mut scene = Scene::new();
    let mut batch_load = BatchLoad::default();
    let mut batch_appends: usize = 0;
    for pass in &frame.passes {
        // A non-source-over blend reads its backdrop, which must be the
        // whole accumulated frame — inside a batch it would only see the
        // content of that batch. Opacity applies to the pass content as a
        // whole, so it stays inline only while the content is one scene.
        let composited = pass.blend != 0 || (pass.opacity < 1.0 && pass.scenes.len() > 1);
        if composited {
            if batch_appends > 0 {
                steps.push(RenderStep::Batch(Box::new(std::mem::replace(
                    &mut scene,
                    Scene::new(),
                ))));
                batch_load = BatchLoad::default();
                batch_appends = 0;
            }
            let mut batches = Vec::new();
            let mut group_scene = Scene::new();
            let mut group_load = BatchLoad::default();
            let mut group_appends: usize = 0;
            for sub_scene in pass.scenes.iter() {
                let load = BatchLoad::of(sub_scene, pass.transform);
                if load.over_capacity() {
                    warn_over_capacity_scene(load);
                }
                if group_appends > 0 && group_load.saturated_by(load) {
                    batches.push(std::mem::replace(&mut group_scene, Scene::new()));
                    group_load = BatchLoad::default();
                    group_appends = 0;
                }
                group_scene.append(&sub_scene.scene, Some(pass.transform));
                group_load.add(load);
                group_appends += 1;
            }
            if group_appends > 0 || batches.is_empty() {
                batches.push(group_scene);
            }
            steps.push(RenderStep::Group {
                batches,
                blend: pass.blend,
                opacity: pass.opacity,
            });
            continue;
        }
        // Isolation without blend or opacity composites as plain source-over,
        // so sub-scenes of such a pass may flatten into shared batches.
        let wrap = pass.isolated || pass.opacity < 1.0;
        for sub_scene in pass.scenes.iter() {
            let load = BatchLoad::of(sub_scene, pass.transform);
            if load.over_capacity() {
                warn_over_capacity_scene(load);
            }
            if batch_appends > 0 && batch_load.saturated_by(load) {
                steps.push(RenderStep::Batch(Box::new(std::mem::replace(
                    &mut scene,
                    Scene::new(),
                ))));
                batch_load = BatchLoad::default();
                batch_appends = 0;
            }
            if wrap {
                scene.push_layer(
                    Fill::NonZero,
                    blend_mode(pass.blend),
                    pass.opacity,
                    Affine::IDENTITY,
                    &viewport,
                );
            }
            scene.append(&sub_scene.scene, Some(pass.transform));
            if wrap {
                scene.pop_layer();
            }
            batch_load.add(load);
            batch_appends += 1;
        }
    }
    if batch_appends > 0 || steps.is_empty() {
        steps.push(RenderStep::Batch(Box::new(scene)));
    }
    Ok(steps)
}

fn acquire_surface_texture(target: &mut SurfaceTarget) -> Result<wgpu::SurfaceTexture, u8> {
    use wgpu::CurrentSurfaceTexture as Current;
    match target.surface.get_current_texture() {
        Current::Success(texture) | Current::Suboptimal(texture) => Ok(texture),
        Current::Outdated => {
            target.surface.configure(&target.device, &target.config);
            match target.surface.get_current_texture() {
                Current::Success(texture) | Current::Suboptimal(texture) => Ok(texture),
                Current::Lost | Current::Outdated | Current::Validation => Err(FRAME_SURFACE_LOST),
                Current::Timeout | Current::Occluded => Err(FRAME_TARGET_UNAVAILABLE),
            }
        }
        Current::Lost | Current::Validation => Err(FRAME_SURFACE_LOST),
        Current::Timeout | Current::Occluded => Err(FRAME_TARGET_UNAVAILABLE),
    }
}

fn clear_color(value: ffi::Color) -> wgpu::Color {
    wgpu::Color {
        r: f64::from(value.red) / 65535.0,
        g: f64::from(value.green) / 65535.0,
        b: f64::from(value.blue) / 65535.0,
        a: f64::from(value.alpha) / 65535.0,
    }
}

struct StepContext<'a> {
    device: &'a wgpu::Device,
    queue: &'a wgpu::Queue,
    renderer: &'a mut VelloRenderer,
    blitter: &'a TextureBlitter,
    blend_blitter: &'a TextureBlitter,
    compositor: &'a mut GroupCompositor,
    render_view: &'a wgpu::TextureView,
    output_view: &'a wgpu::TextureView,
    width: u32,
    height: u32,
}

fn render_steps(
    ctx: &mut StepContext,
    background: ffi::Color,
    steps: &[RenderStep],
) -> Result<(), vello::Error> {
    // A leading plain batch lays down the frame background as its vello base
    // color, compositing its content at full precision as one submission.
    // Otherwise — the first step is a blended group — the background is
    // cleared first so the group composites onto initialized frame pixels.
    let first_batch_carries_background = matches!(steps.first(), Some(RenderStep::Batch(_)));
    if !first_batch_carries_background {
        let mut encoder = ctx
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("Mapper Vello background"),
            });
        encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("Mapper Vello background"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: ctx.output_view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(clear_color(background)),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        ctx.queue.submit(Some(encoder.finish()));
    }

    let params = RenderParams {
        base_color: Color::TRANSPARENT,
        width: ctx.width,
        height: ctx.height,
        antialiasing_method: AaConfig::Area,
    };
    for (index, step) in steps.iter().enumerate() {
        match step {
            RenderStep::Batch(scene) => {
                let carries_background = index == 0 && first_batch_carries_background;
                let batch_params = RenderParams {
                    base_color: if carries_background {
                        color(background, 1.0)
                    } else {
                        Color::TRANSPARENT
                    },
                    ..params
                };
                ctx.renderer.render_to_texture(
                    ctx.device,
                    ctx.queue,
                    scene,
                    ctx.render_view,
                    &batch_params,
                )?;
                let mut encoder =
                    ctx.device
                        .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                            label: Some("Mapper Vello batch composite"),
                        });
                let blitter = if carries_background {
                    ctx.blitter
                } else {
                    ctx.blend_blitter
                };
                blitter.copy(ctx.device, &mut encoder, ctx.render_view, ctx.output_view);
                ctx.queue.submit(Some(encoder.finish()));
            }
            RenderStep::Group {
                batches,
                blend,
                opacity,
            } => {
                // The group texture accumulates the pass content as
                // premultiplied color over transparent: each batch's
                // straight-alpha vello output blends in with straight-alpha
                // source factors.
                let group_view = ctx
                    .compositor
                    .target_view(ctx.device, ctx.width, ctx.height);
                let mut encoder =
                    ctx.device
                        .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                            label: Some("Mapper Vello group clear"),
                        });
                encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("Mapper Vello group clear"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &group_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                ctx.queue.submit(Some(encoder.finish()));
                for scene in batches {
                    ctx.renderer.render_to_texture(
                        ctx.device,
                        ctx.queue,
                        scene,
                        ctx.render_view,
                        &params,
                    )?;
                    let mut encoder =
                        ctx.device
                            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                                label: Some("Mapper Vello group accumulate"),
                            });
                    ctx.blend_blitter
                        .copy(ctx.device, &mut encoder, ctx.render_view, &group_view);
                    ctx.queue.submit(Some(encoder.finish()));
                }
                ctx.compositor
                    .composite(ctx.device, ctx.queue, ctx.output_view, *blend, *opacity);
            }
        }
    }
    Ok(())
}

fn render_frame(target: &mut SurfaceTarget, frame: &FrameBuilder) -> Result<(), u8> {
    let width = frame.header.width.max(1);
    let height = frame.header.height.max(1);
    target.resize(width, height);
    let output = acquire_surface_texture(target)?;
    let output_view = output
        .texture
        .create_view(&wgpu::TextureViewDescriptor::default());
    let steps = compose_steps(frame).map_err(|_| FRAME_ERROR)?;
    let render_view = target.render_texture(width, height).clone();
    let mut ctx = StepContext {
        device: &target.device,
        queue: &target.queue,
        renderer: &mut target.renderer,
        blitter: &target.blitter,
        blend_blitter: &target.blend_blitter,
        compositor: &mut target.compositor,
        render_view: &render_view,
        output_view: &output_view,
        width,
        height,
    };
    render_steps(&mut ctx, frame.header.background, &steps).map_err(|_| FRAME_ERROR)?;
    output.present();
    Ok(())
}

fn render_offscreen(target: &mut OffscreenTarget, frame: &FrameBuilder) -> Result<Vec<u8>, String> {
    let width = frame.header.width.max(1);
    let height = frame.header.height.max(1);
    let steps = compose_steps(frame).map_err(str::to_owned)?;
    let recreate = target
        .resources
        .as_ref()
        .is_none_or(|resources| resources.width != width || resources.height != height);
    if recreate {
        let unpadded_bytes_per_row = width
            .checked_mul(4)
            .ok_or("offscreen row byte count overflow")?;
        let alignment = wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
        let padded_bytes_per_row = unpadded_bytes_per_row.div_ceil(alignment) * alignment;
        let buffer_size = u64::from(padded_bytes_per_row)
            .checked_mul(u64::from(height))
            .ok_or("offscreen readback byte count overflow")?;
        let texture = target.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("Mapper Vello offscreen target"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        let render_texture = target.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("Mapper Vello offscreen frame target"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let render_view = render_texture.create_view(&wgpu::TextureViewDescriptor::default());
        let buffer = target.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("Mapper Vello offscreen readback"),
            size: buffer_size,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        target.resources = Some(OffscreenResources {
            texture,
            view,
            _render_texture: render_texture,
            render_view,
            buffer,
            width,
            height,
            unpadded_bytes_per_row,
            padded_bytes_per_row,
        });
    }
    let (render_view, output_view) = {
        let resources = target
            .resources
            .as_ref()
            .expect("offscreen resources exist");
        (resources.render_view.clone(), resources.view.clone())
    };
    let mut ctx = StepContext {
        device: &target.device,
        queue: &target.queue,
        renderer: &mut target.renderer,
        blitter: &target.blitter,
        blend_blitter: &target.blend_blitter,
        compositor: &mut target.compositor,
        render_view: &render_view,
        output_view: &output_view,
        width,
        height,
    };
    render_steps(&mut ctx, frame.header.background, &steps)
        .map_err(|error| format!("Vello offscreen render failed: {error}"))?;
    let resources = target
        .resources
        .as_ref()
        .expect("offscreen resources exist");
    let mut encoder = target
        .device
        .create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("Mapper Vello offscreen readback"),
        });
    encoder.copy_texture_to_buffer(
        wgpu::TexelCopyTextureInfo {
            texture: &resources.texture,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        wgpu::TexelCopyBufferInfo {
            buffer: &resources.buffer,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(resources.padded_bytes_per_row),
                rows_per_image: Some(height),
            },
        },
        wgpu::Extent3d {
            width,
            height,
            depth_or_array_layers: 1,
        },
    );
    target.queue.submit(Some(encoder.finish()));

    let slice = resources.buffer.slice(..);
    let (mapped_tx, mapped_rx) = std::sync::mpsc::sync_channel(1);
    slice.map_async(wgpu::MapMode::Read, move |result| {
        let _ = mapped_tx.send(result);
    });
    target
        .device
        .poll(wgpu::PollType::wait_indefinitely())
        .map_err(|error| format!("offscreen GPU wait failed: {error}"))?;
    mapped_rx
        .recv()
        .map_err(|_| "offscreen readback callback disconnected".to_owned())?
        .map_err(|error| format!("offscreen readback mapping failed: {error}"))?;
    let mapped = slice.get_mapped_range();
    let output_size = usize::try_from(resources.unpadded_bytes_per_row)
        .ok()
        .and_then(|row| row.checked_mul(height as usize))
        .ok_or("offscreen output byte count overflow")?;
    let mut output = Vec::with_capacity(output_size);
    for row in mapped
        .chunks_exact(resources.padded_bytes_per_row as usize)
        .take(height as usize)
    {
        output.extend_from_slice(&row[..resources.unpadded_bytes_per_row as usize]);
    }
    drop(mapped);
    resources.buffer.unmap();
    Ok(output)
}

enum RenderEvent {
    Control(Result<Control, flume::RecvError>),
    Frame(Result<Box<FrameBuilder>, flume::RecvError>),
    Offscreen(Result<OffscreenRequest, flume::RecvError>),
}

fn apply_surface_state(
    update: SurfaceUpdate,
    current: &mut ffi::SurfaceState,
    target: &mut Option<SurfaceTarget>,
    last_error: &Mutex<String>,
    prepared_descriptor: &Mutex<Option<(u8, u64, u64)>>,
) {
    let state = update.state;
    if state.sequence <= current.sequence {
        return;
    }
    *current = state;
    if state.phase == SURFACE_UNAVAILABLE {
        *target = None;
        return;
    }
    if state.phase != SURFACE_EXPOSED || state.width == 0 || state.height == 0 {
        return;
    }
    if let Some(existing) = target.as_mut().filter(|existing| existing.matches(state)) {
        existing.resize(state.width, state.height);
        return;
    }
    let Some(prepared) = update.prepared else {
        *target = None;
        set_error(
            last_error,
            "native surface was not prepared on the Qt platform thread",
        );
        if let Ok(mut descriptor) = prepared_descriptor.lock() {
            *descriptor = None;
        }
        return;
    };
    match prepared.and_then(|prepared| create_surface_target(state, prepared)) {
        Ok(created) => {
            *target = Some(created);
            set_error(last_error, "");
        }
        Err(error) => {
            *target = None;
            set_error(last_error, error);
            if let Ok(mut descriptor) = prepared_descriptor.lock() {
                *descriptor = None;
            }
        }
    }
}

fn process_frame(
    frame: Box<FrameBuilder>,
    current: ffi::SurfaceState,
    target: &mut Option<SurfaceTarget>,
    result_tx: &Sender<ffi::FrameResult>,
    result_replace_rx: &Receiver<ffi::FrameResult>,
    last_error: &Mutex<String>,
    prepared_descriptor: &Mutex<Option<(u8, u64, u64)>>,
) {
    if frame.header.surface_sequence != current.sequence {
        publish_result(
            result_tx,
            result_replace_rx,
            frame_result(&frame, FRAME_DROPPED_STALE),
        );
        return;
    }
    if current.phase != SURFACE_EXPOSED || current.width == 0 || current.height == 0 {
        publish_result(
            result_tx,
            result_replace_rx,
            frame_result(&frame, FRAME_TARGET_UNAVAILABLE),
        );
        return;
    }
    if target
        .as_ref()
        .is_none_or(|target| !target.matches(current))
    {
        set_error(
            last_error,
            "Vello frame arrived without a prepared native surface target",
        );
        if let Ok(mut descriptor) = prepared_descriptor.lock() {
            *descriptor = None;
        }
        publish_result(
            result_tx,
            result_replace_rx,
            // No presentation target is available for this otherwise valid
            // lifecycle state. This is distinct from losing an established
            // wgpu surface: callers may retain the current lifecycle and
            // retry without manufacturing resize/expose transitions.
            frame_result(&frame, FRAME_TARGET_UNAVAILABLE),
        );
        return;
    }

    let target_ref = target.as_mut().expect("surface target was created");
    let status = match render_frame(target_ref, &frame) {
        Ok(()) => FRAME_PRESENTED,
        Err(status) => {
            if status == FRAME_SURFACE_LOST {
                *target = None;
                if let Ok(mut descriptor) = prepared_descriptor.lock() {
                    *descriptor = None;
                }
            }
            status
        }
    };
    if status == FRAME_ERROR {
        set_error(last_error, "Vello failed to render the submitted frame");
    }
    publish_result(result_tx, result_replace_rx, frame_result(&frame, status));
}

fn render_thread(
    control_rx: Receiver<Control>,
    frame_rx: Receiver<Box<FrameBuilder>>,
    offscreen_rx: Receiver<OffscreenRequest>,
    result_tx: Sender<ffi::FrameResult>,
    result_replace_rx: Receiver<ffi::FrameResult>,
    last_error: Arc<Mutex<String>>,
    prepared_descriptor: Arc<Mutex<Option<(u8, u64, u64)>>>,
) {
    let mut current = ffi::SurfaceState::default();
    let mut target = None;
    let mut offscreen_target = None;
    loop {
        while let Ok(control) = control_rx.try_recv() {
            match control {
                Control::SetSurface(update) => {
                    apply_surface_state(
                        update,
                        &mut current,
                        &mut target,
                        &last_error,
                        &prepared_descriptor,
                    );
                }
                Control::Shutdown => return,
            }
        }
        match frame_rx.try_recv() {
            Ok(frame) => {
                process_frame(
                    frame,
                    current,
                    &mut target,
                    &result_tx,
                    &result_replace_rx,
                    &last_error,
                    &prepared_descriptor,
                );
                continue;
            }
            Err(TryRecvError::Disconnected) => return,
            Err(TryRecvError::Empty) => {}
        }

        if let Ok(request) = offscreen_rx.try_recv() {
            let result = (|| {
                if offscreen_target.is_none() {
                    offscreen_target = Some(create_offscreen_target()?);
                }
                render_offscreen(
                    offscreen_target.as_mut().expect("offscreen target exists"),
                    &request.frame,
                )
            })();
            let _ = request.response.send(result);
            continue;
        }

        let event = flume::Selector::new()
            .recv(&control_rx, RenderEvent::Control)
            .recv(&frame_rx, RenderEvent::Frame)
            .recv(&offscreen_rx, RenderEvent::Offscreen)
            .wait();
        match event {
            RenderEvent::Control(Ok(Control::SetSurface(update))) => {
                apply_surface_state(
                    update,
                    &mut current,
                    &mut target,
                    &last_error,
                    &prepared_descriptor,
                );
            }
            RenderEvent::Control(Ok(Control::Shutdown))
            | RenderEvent::Control(Err(_))
            | RenderEvent::Frame(Err(_))
            | RenderEvent::Offscreen(Err(_)) => return,
            RenderEvent::Frame(Ok(frame)) => process_frame(
                frame,
                current,
                &mut target,
                &result_tx,
                &result_replace_rx,
                &last_error,
                &prepared_descriptor,
            ),
            RenderEvent::Offscreen(Ok(request)) => {
                let result = (|| {
                    if offscreen_target.is_none() {
                        offscreen_target = Some(create_offscreen_target()?);
                    }
                    render_offscreen(
                        offscreen_target.as_mut().expect("offscreen target exists"),
                        &request.frame,
                    )
                })();
                let _ = request.response.send(result);
            }
        }
    }
}

fn new_renderer() -> Box<Renderer> {
    let (control_tx, control_rx) = flume::bounded(32);
    let (frame_tx, frame_rx) = flume::bounded(1);
    let frame_replace_rx = frame_rx.clone();
    let (offscreen_tx, offscreen_rx) = flume::bounded(1);
    let (result_tx, result_rx) = flume::bounded(64);
    let result_replace_rx = result_rx.clone();
    let last_error = Arc::new(Mutex::new(String::new()));
    let thread_error = Arc::clone(&last_error);
    let prepared_descriptor = Arc::new(Mutex::new(None));
    let thread_prepared_descriptor = Arc::clone(&prepared_descriptor);
    let thread = thread::Builder::new()
        .name("mapper-vello".to_owned())
        .spawn(move || {
            render_thread(
                control_rx,
                frame_rx,
                offscreen_rx,
                result_tx,
                result_replace_rx,
                thread_error,
                thread_prepared_descriptor,
            );
        })
        .ok();
    if thread.is_none() {
        set_error(&last_error, "failed to start the Vello render thread");
    }
    Box::new(Renderer {
        control_tx,
        frame_tx,
        frame_replace_rx,
        offscreen_tx,
        result_rx,
        last_error,
        prepared_descriptor,
        thread: Mutex::new(thread),
    })
}

fn renderer_set_surface(renderer: &Renderer, state: ffi::SurfaceState) -> bool {
    let descriptor = (state.platform, state.window, state.display);
    let current_descriptor = renderer
        .prepared_descriptor
        .lock()
        .ok()
        .and_then(|descriptor| *descriptor);
    let should_prepare = state.phase == SURFACE_EXPOSED
        && state.width > 0
        && state.height > 0
        && current_descriptor != Some(descriptor);
    let prepared = should_prepare.then(|| prepare_surface(state));
    let prepared_successfully = prepared.as_ref().is_some_and(Result::is_ok);
    match renderer
        .control_tx
        .try_send(Control::SetSurface(SurfaceUpdate { state, prepared }))
    {
        Ok(()) => {
            if let Ok(mut current) = renderer.prepared_descriptor.lock() {
                if state.phase == SURFACE_UNAVAILABLE {
                    *current = None;
                } else if prepared_successfully {
                    *current = Some(descriptor);
                }
            }
            true
        }
        Err(TrySendError::Full(_)) => {
            set_error(
                &renderer.last_error,
                "Vello lifecycle queue exhausted before the render thread drained it",
            );
            false
        }
        Err(TrySendError::Disconnected(_)) => {
            set_error(&renderer.last_error, "Vello render thread is not running");
            false
        }
    }
}

fn submit_latest_frame(
    frame_tx: &Sender<Box<FrameBuilder>>,
    frame_replace_rx: &Receiver<Box<FrameBuilder>>,
    frame: Box<FrameBuilder>,
) -> bool {
    match frame_tx.try_send(frame) {
        Ok(()) => true,
        Err(TrySendError::Full(frame)) => {
            let _ = frame_replace_rx.try_recv();
            frame_tx.try_send(frame).is_ok()
        }
        Err(TrySendError::Disconnected(_)) => false,
    }
}

fn renderer_submit(renderer: &Renderer, frame: Box<FrameBuilder>) -> bool {
    let accepted = submit_latest_frame(&renderer.frame_tx, &renderer.frame_replace_rx, frame);
    if !accepted {
        set_error(&renderer.last_error, "Vello render thread is not running");
    }
    accepted
}

fn renderer_render_offscreen(renderer: &Renderer, frame: Box<FrameBuilder>) -> Vec<u8> {
    let (response_tx, response_rx) = flume::bounded(1);
    if renderer
        .offscreen_tx
        .send(OffscreenRequest {
            frame,
            response: response_tx,
        })
        .is_err()
    {
        set_error(&renderer.last_error, "Vello render thread is not running");
        return Vec::new();
    }
    match response_rx.recv() {
        Ok(Ok(pixels)) => {
            set_error(&renderer.last_error, "");
            pixels
        }
        Ok(Err(error)) => {
            set_error(&renderer.last_error, error);
            Vec::new()
        }
        Err(_) => {
            set_error(
                &renderer.last_error,
                "Vello render thread stopped before offscreen rendering completed",
            );
            Vec::new()
        }
    }
}

fn renderer_try_take_result(renderer: &Renderer) -> ffi::FrameResult {
    renderer.result_rx.try_recv().unwrap_or(ffi::FrameResult {
        status: FRAME_NONE,
        ..ffi::FrameResult::default()
    })
}

fn renderer_last_error(renderer: &Renderer) -> String {
    renderer
        .last_error
        .lock()
        .map(|error| error.clone())
        .unwrap_or_else(|_| "Vello renderer error state was poisoned".to_owned())
}

impl Drop for Renderer {
    fn drop(&mut self) {
        let _ = self.control_tx.send(Control::Shutdown);
        if let Ok(mut thread) = self.thread.lock()
            && let Some(thread) = thread.take()
        {
            let _ = thread.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(id: u64) -> Box<FrameBuilder> {
        new_frame(ffi::FrameHeader {
            frame_id: id,
            revision: 1,
            width: 1,
            height: 1,
            ..ffi::FrameHeader::default()
        })
    }

    fn zigzag_path(lines: usize) -> Vec<ffi::PathElement> {
        let mut elements = vec![ffi::PathElement {
            verb: 0,
            ..ffi::PathElement::default()
        }];
        for index in 0..lines {
            elements.push(ffi::PathElement {
                verb: 1,
                x1: index as f64,
                y1: (index % 2) as f64,
                ..ffi::PathElement::default()
            });
        }
        elements
    }

    /// Records `fills` fill commands of `lines_each` segments at the top
    /// level of a fresh scene and returns the retained result.
    fn recorded_fills_at(fills: usize, lines_each: usize, scale_hint: f64) -> Box<RetainedScene> {
        let mut builder = begin_scene(1, ffi::Rect::default(), scale_hint);
        let path = zigzag_path(lines_each);
        for _ in 0..fills {
            scene_fill_path(&mut builder, 1, &path, ffi::Color::default());
        }
        finish_scene(builder)
    }

    fn recorded_fills(fills: usize, lines_each: usize) -> Box<RetainedScene> {
        recorded_fills_at(fills, lines_each, 1.0)
    }

    #[test]
    fn recording_splits_over_budget_scenes_at_top_level() {
        let budget = usize::try_from(BATCH_ENCODED_SEGMENT_BUDGET).unwrap();
        let lines_each = 10_000;
        let fills = 2 * budget / lines_each + 2;
        let retained = recorded_fills(fills, lines_each);
        assert!(
            retained.scenes.len() >= 2,
            "{} sub-scenes",
            retained.scenes.len()
        );
        assert_eq!(retained.command_count, fills as u64);
        for sub_scene in retained.scenes.iter() {
            let segments = u64::from(sub_scene.scene.encoding().n_path_segments);
            assert!(
                segments <= BATCH_ENCODED_SEGMENT_BUDGET + 2 * lines_each as u64,
                "sub-scene encodes {segments} segments"
            );
        }
    }

    #[test]
    fn recording_never_splits_inside_clip_groups() {
        let budget = usize::try_from(BATCH_ENCODED_SEGMENT_BUDGET).unwrap();
        let lines_each = 10_000;
        let fills = budget / lines_each + 2;
        let mut builder = begin_scene(1, ffi::Rect::default(), 1.0);
        let path = zigzag_path(lines_each);
        scene_push_clip(&mut builder, 1, &path);
        for _ in 0..fills {
            scene_fill_path(&mut builder, 1, &path, ffi::Color::default());
        }
        scene_pop_clip(&mut builder);
        let retained = finish_scene(builder);
        assert_eq!(retained.scenes.len(), 1);
    }

    #[test]
    fn split_blended_pass_becomes_a_composited_group() {
        let budget = usize::try_from(BATCH_ENCODED_SEGMENT_BUDGET).unwrap();
        let lines_each = 10_000;
        let fills = 2 * budget / lines_each + 2;
        let split = recorded_fills(fills, lines_each);
        assert!(split.scenes.len() >= 2);
        let small = recorded_fills(1, 8);

        let mut builder = frame(1);
        frame_add_pass(
            &mut builder,
            &small,
            0,
            1.0,
            false,
            ffi::Transform {
                m11: 1.0,
                m22: 1.0,
                ..ffi::Transform::default()
            },
        );
        frame_add_pass(
            &mut builder,
            &split,
            1,
            1.0,
            true,
            ffi::Transform {
                m11: 1.0,
                m22: 1.0,
                ..ffi::Transform::default()
            },
        );
        let steps = compose_steps(&builder).unwrap();
        assert_eq!(steps.len(), 2);
        assert!(matches!(steps[0], RenderStep::Batch(_)));
        match &steps[1] {
            RenderStep::Group {
                batches,
                blend,
                opacity,
            } => {
                assert!(
                    batches.len() >= 2 && batches.len() <= split.scenes.len(),
                    "{} batches for {} sub-scenes",
                    batches.len(),
                    split.scenes.len()
                );
                assert_eq!(*blend, 1);
                assert_eq!(*opacity, 1.0);
            }
            RenderStep::Batch(_) => panic!("split blended pass was not grouped"),
        }
    }

    #[test]
    fn split_plain_pass_flattens_into_bounded_batches() {
        let budget = usize::try_from(BATCH_ENCODED_SEGMENT_BUDGET).unwrap();
        let lines_each = 10_000;
        let fills = 2 * budget / lines_each + 2;
        let split = recorded_fills(fills, lines_each);
        assert!(split.scenes.len() >= 2);

        let mut builder = frame(1);
        frame_add_pass(
            &mut builder,
            &split,
            0,
            1.0,
            false,
            ffi::Transform {
                m11: 1.0,
                m22: 1.0,
                ..ffi::Transform::default()
            },
        );
        let steps = compose_steps(&builder).unwrap();
        assert!(
            steps.len() >= 2 && steps.len() <= split.scenes.len(),
            "{} steps for {} sub-scenes",
            steps.len(),
            split.scenes.len()
        );
        for step in &steps {
            match step {
                RenderStep::Batch(scene) => {
                    let segments = u64::from(scene.encoding().n_path_segments);
                    assert!(
                        segments <= BATCH_ENCODED_SEGMENT_BUDGET + 2 * lines_each as u64,
                        "batch encodes {segments} segments"
                    );
                }
                RenderStep::Group { .. } => panic!("plain pass must not need compositing"),
            }
        }
    }

    #[test]
    fn high_scale_hint_splits_recording_finer() {
        let coarse = recorded_fills_at(60, 1_000, 1.0);
        let fine = recorded_fills_at(60, 1_000, 1_000.0);
        assert_eq!(coarse.scenes.len(), 1);
        assert!(
            fine.scenes.len() > coarse.scenes.len(),
            "hint 1000 produced {} sub-scenes",
            fine.scenes.len()
        );
    }

    #[test]
    fn high_zoom_transform_shrinks_batches() {
        let split = recorded_fills_at(60, 1_000, 1_000.0);
        assert!(split.scenes.len() >= 2);
        let compose_at = |scale: f64| {
            let mut builder = frame(1);
            frame_add_pass(
                &mut builder,
                &split,
                0,
                1.0,
                false,
                ffi::Transform {
                    m11: scale,
                    m22: scale,
                    ..ffi::Transform::default()
                },
            );
            compose_steps(&builder).unwrap().len()
        };
        assert!(
            compose_at(1_000.0) > compose_at(1.0),
            "batches: {} at 1000x vs {} at 1x",
            compose_at(1_000.0),
            compose_at(1.0)
        );
    }

    #[test]
    fn bounded_frame_channel_keeps_latest() {
        let (frame_tx, frame_rx) = flume::bounded(1);
        let frame_replace_rx = frame_rx.clone();

        for id in 1..=129 {
            assert!(submit_latest_frame(&frame_tx, &frame_replace_rx, frame(id),));
        }

        assert_eq!(frame_rx.recv().unwrap().header.frame_id, 129);
    }

    #[test]
    fn presentation_avoids_a_second_srgb_encoding() {
        let formats = [
            wgpu::TextureFormat::Bgra8UnormSrgb,
            wgpu::TextureFormat::Rgba16Float,
            wgpu::TextureFormat::Bgra8Unorm,
            wgpu::TextureFormat::Rgba8Unorm,
        ];

        assert_eq!(
            presentation_format(&formats),
            Some(wgpu::TextureFormat::Bgra8Unorm)
        );
        assert_eq!(
            presentation_format(&[wgpu::TextureFormat::Rgba8UnormSrgb]),
            None
        );
    }

    #[test]
    fn missing_native_target_is_retriable_without_surface_churn() {
        let (result_tx, result_rx) = flume::bounded(2);
        let result_replace_rx = result_rx.clone();
        let last_error = Mutex::new(String::new());
        let prepared_descriptor = Mutex::new(Some((PLATFORM_XCB, 1, 2)));
        let mut submitted = frame(7);
        submitted.header.surface_sequence = 11;

        process_frame(
            submitted,
            ffi::SurfaceState {
                sequence: 11,
                phase: SURFACE_EXPOSED,
                width: 1,
                height: 1,
                ..ffi::SurfaceState::default()
            },
            &mut None,
            &result_tx,
            &result_replace_rx,
            &last_error,
            &prepared_descriptor,
        );

        assert_eq!(result_rx.recv().unwrap().status, FRAME_TARGET_UNAVAILABLE);
        assert_eq!(*prepared_descriptor.lock().unwrap(), None);
    }
}
