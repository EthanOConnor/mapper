use std::collections::HashMap;
use std::num::{NonZeroIsize, NonZeroU32};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Instant;

use flume::{Receiver, Sender, TryRecvError, TrySendError};
use raw_window_handle::{
    AndroidDisplayHandle, AndroidNdkWindowHandle, AppKitDisplayHandle, AppKitWindowHandle,
    RawDisplayHandle, RawWindowHandle, UiKitDisplayHandle, UiKitWindowHandle, WaylandDisplayHandle,
    WaylandWindowHandle, Win32WindowHandle, WindowsDisplayHandle, XcbDisplayHandle,
    XcbWindowHandle,
};
use vello::kurbo::{Affine, BezPath, Cap, Ellipse, Join, Rect, Stroke};
use vello::peniko::{
    BlendMode, Blob, Color, ColorStop, Compose, Extend, Fill, FontData, Gradient, ImageAlphaType,
    ImageBrush, ImageData, ImageFormat, ImageQuality, ImageSampler, Mix,
};
use vello::wgpu;
use vello::wgpu::util::TextureBlitter;
use vello::{AaConfig, AaSupport, Glyph as VelloGlyph, RenderParams, Renderer as VelloRenderer};
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
    struct Glyph {
        id: u32,
        x: f64,
        y: f64,
    }

    #[derive(Clone, Copy, Default)]
    struct GlyphStyle {
        content_id: u64,
        size: f64,
        transform: Transform,
        color: Color,
        stroke: Stroke,
        outline: bool,
        hint: bool,
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
        backend: u8,
        scene_count: u32,
        render_cpu_us: u64,
    }

    extern "Rust" {
        type SceneEncoder;
        type SceneBuilder;
        type RetainedScene;
        type RetainedImage;
        type FrameBuilder;
        type Renderer;

        fn new_scene_encoder() -> Box<SceneEncoder>;
        fn begin_scene(revision: u64, world_bounds: Rect) -> Box<SceneBuilder>;
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
        fn scene_draw_glyphs(
            encoder: &mut SceneEncoder,
            scene: &mut SceneBuilder,
            font_bytes: &[u8],
            glyphs: &[Glyph],
            style: GlyphStyle,
        ) -> bool;
        fn new_retained_image(
            image_bytes: &[u8],
            width: u32,
            height: u32,
            stride: u32,
        ) -> Box<RetainedImage>;
        fn scene_draw_image(
            scene: &mut SceneBuilder,
            image: &RetainedImage,
            target: Rect,
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
const PLATFORM_UIKIT: u8 = 2;
const PLATFORM_WIN32: u8 = 3;
const PLATFORM_ANDROID_NDK: u8 = 4;
const PLATFORM_WAYLAND: u8 = 5;
const PLATFORM_XCB: u8 = 6;

const FRAME_NONE: u8 = 0;
const FRAME_PRESENTED: u8 = 1;
const FRAME_TARGET_UNAVAILABLE: u8 = 2;
const FRAME_DROPPED_STALE: u8 = 3;
const FRAME_SURFACE_LOST: u8 = 4;
const FRAME_ERROR: u8 = 5;

pub struct SceneEncoder {
    fonts: HashMap<u64, FontData>,
}

pub struct SceneBuilder {
    scene: Scene,
    revision: u64,
    command_count: u64,
    transforms: Vec<Affine>,
    world_bounds: Rect,
}

pub struct RetainedScene {
    scene: Arc<Scene>,
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
    scene: Arc<Scene>,
    blend: u8,
    opacity: f32,
    isolated: bool,
    transform: Affine,
}

struct OffscreenRequest {
    frame: Box<FrameBuilder>,
    response: Sender<Result<Vec<u8>, String>>,
}

fn new_scene_encoder() -> Box<SceneEncoder> {
    Box::new(SceneEncoder {
        fonts: HashMap::new(),
    })
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

impl SceneBuilder {
    fn transform(&self) -> Affine {
        self.transforms.last().copied().unwrap_or(Affine::IDENTITY)
    }

    fn viewport(&self) -> Rect {
        self.world_bounds
    }
}

fn begin_scene(revision: u64, world_bounds: ffi::Rect) -> Box<SceneBuilder> {
    Box::new(SceneBuilder {
        scene: Scene::new(),
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
    if let Some(path) = path(elements) {
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
    scene.scene.pop_layer();
}

fn scene_push_layer(scene: &mut SceneBuilder, opacity: f64, blend: u8) {
    scene.command_count += 1;
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
    scene.scene.pop_layer();
}

fn scene_fill_path(
    scene: &mut SceneBuilder,
    rule: u8,
    elements: &[ffi::PathElement],
    brush: ffi::Color,
) {
    scene.command_count += 1;
    if let Some(path) = path(elements) {
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
    let valid_dashes =
        dash_offset.is_finite() && dashes.iter().all(|value| value.is_finite() && *value > 0.0);
    if let (Some(path), Some(mut stroke)) = (path(elements), stroke(style))
        && valid_dashes
    {
        stroke.dash_pattern.extend_from_slice(dashes);
        stroke.dash_offset = dash_offset;
        scene
            .scene
            .stroke(&stroke, scene.transform(), color(brush, 1.0), None, &path);
    }
}

fn scene_fill_ellipse(scene: &mut SceneBuilder, bounds: ffi::Rect, brush: ffi::Color) {
    scene.command_count += 1;
    if let Some(bounds) = rect(bounds) {
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
    if let (Some(bounds), Some(stroke)) = (rect(bounds), stroke(style)) {
        scene.scene.stroke(
            &stroke,
            scene.transform(),
            color(brush, 1.0),
            None,
            &Ellipse::from_rect(bounds),
        );
    }
}

fn scene_draw_glyphs(
    encoder: &mut SceneEncoder,
    scene: &mut SceneBuilder,
    font_bytes: &[u8],
    glyphs: &[ffi::Glyph],
    style: ffi::GlyphStyle,
) -> bool {
    scene.command_count += 1;
    if style.content_id == 0
        || font_bytes.is_empty()
        || glyphs.is_empty()
        || !style.size.is_finite()
        || style.size <= 0.0
    {
        return false;
    }
    let Some(run_transform) = finite_transform(style.transform) else {
        return false;
    };
    let mut run = Vec::with_capacity(glyphs.len());
    for glyph in glyphs {
        if !glyph.x.is_finite() || !glyph.y.is_finite() {
            return false;
        }
        run.push(VelloGlyph {
            id: glyph.id,
            x: glyph.x as f32,
            y: glyph.y as f32,
        });
    }
    let font = encoder
        .fonts
        .entry(style.content_id)
        .or_insert_with(|| FontData::new(Blob::from(font_bytes.to_vec()), 0));
    let transform = scene.transform() * run_transform;
    let builder = scene
        .scene
        .draw_glyphs(font)
        .transform(transform)
        .font_size(style.size as f32)
        .hint(style.hint)
        .brush(color(style.color, 1.0));
    if style.outline {
        let Some(stroke) = stroke(style.stroke) else {
            return false;
        };
        builder.draw(&stroke, run.into_iter());
    } else {
        builder.draw(Fill::NonZero, run.into_iter());
    }
    true
}

fn new_retained_image(
    image_bytes: &[u8],
    width: u32,
    height: u32,
    stride: u32,
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
            alpha_type: ImageAlphaType::Alpha,
            width,
            height,
        }),
    })
}

fn scene_draw_image(
    scene: &mut SceneBuilder,
    retained: &RetainedImage,
    target: ffi::Rect,
    opacity: f64,
) -> bool {
    scene.command_count += 1;
    let Some(target) = rect(target) else {
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
    let brush = ImageBrush {
        image,
        sampler: ImageSampler {
            x_extend: Extend::Pad,
            y_extend: Extend::Pad,
            quality: ImageQuality::Medium,
            alpha: opacity.clamp(0.0, 1.0) as f32,
        },
    };
    let image_to_target = Affine::translate((target.x0, target.y0))
        * Affine::scale_non_uniform(
            target.width() / f64::from(width),
            target.height() / f64::from(height),
        );
    scene
        .scene
        .draw_image(&brush, scene.transform() * image_to_target);
    true
}

fn scene_draw_line_pattern(
    scene: &mut SceneBuilder,
    rule: u8,
    outline: &[ffi::PathElement],
    style: ffi::LinePatternStyle,
) {
    scene.command_count += 1;
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
    Box::new(RetainedScene {
        scene: Arc::new(scene.scene),
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
        scene: Arc::clone(&scene.scene),
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
    config: wgpu::SurfaceConfiguration,
    render_texture: Option<RenderTexture>,
    platform: u8,
    window: u64,
    display: u64,
    backend: u8,
}

struct OffscreenTarget {
    _instance: wgpu::Instance,
    device: wgpu::Device,
    queue: wgpu::Queue,
    renderer: VelloRenderer,
    resources: Option<OffscreenResources>,
}

struct OffscreenResources {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
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
        PLATFORM_UIKIT => {
            let view = raw_handle(state.window).ok_or("UIKit surface has no UIView")?;
            Ok((
                Some(UiKitDisplayHandle::new().into()),
                UiKitWindowHandle::new(view).into(),
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

fn backend_id(backend: wgpu::Backend) -> u8 {
    match backend {
        wgpu::Backend::Vulkan => 1,
        wgpu::Backend::Metal => 2,
        wgpu::Backend::Dx12 => 3,
        wgpu::Backend::Gl => 4,
        wgpu::Backend::BrowserWebGpu => 5,
        wgpu::Backend::Noop => 6,
    }
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
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        force_fallback_adapter: false,
        compatible_surface: Some(&surface),
    }))
    .map_err(|error| format!("no compatible wgpu adapter: {error}"))?;
    let backend = backend_id(adapter.get_info().backend);
    let (device, queue) = pollster::block_on(adapter.request_device(&device_descriptor(&adapter)))
        .map_err(|error| format!("wgpu device creation failed: {error}"))?;
    let capabilities = surface.get_capabilities(&adapter);
    let mut config = surface
        .get_default_config(&adapter, state.width, state.height)
        .ok_or("wgpu surface has no supported default configuration")?;
    config.usage = wgpu::TextureUsages::RENDER_ATTACHMENT;
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
    Ok(SurfaceTarget {
        _instance: instance,
        surface,
        device,
        queue,
        renderer,
        blitter,
        config,
        render_texture: None,
        platform: state.platform,
        window: state.window,
        display: state.display,
        backend,
    })
}

fn create_offscreen_target() -> Result<OffscreenTarget, String> {
    let instance = wgpu::Instance::new(instance_descriptor(wgpu::Backends::PRIMARY));
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        force_fallback_adapter: false,
        compatible_surface: None,
    }))
    .map_err(|error| format!("no headless wgpu adapter: {error}"))?;
    let (device, queue) = pollster::block_on(adapter.request_device(&device_descriptor(&adapter)))
        .map_err(|error| format!("headless wgpu device creation failed: {error}"))?;
    let renderer = VelloRenderer::new(
        &device,
        RendererOptions {
            antialiasing_support: AaSupport::area_only(),
            ..RendererOptions::default()
        },
    )
    .map_err(|error| format!("headless Vello renderer creation failed: {error}"))?;
    Ok(OffscreenTarget {
        _instance: instance,
        device,
        queue,
        renderer,
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

fn frame_result(
    frame: &FrameBuilder,
    status: u8,
    backend: u8,
    started: Instant,
) -> ffi::FrameResult {
    ffi::FrameResult {
        frame_id: frame.header.frame_id,
        revision: frame.header.revision,
        surface_sequence: frame.header.surface_sequence,
        status,
        backend,
        scene_count: frame.passes.len().min(u32::MAX as usize) as u32,
        render_cpu_us: started.elapsed().as_micros().min(u128::from(u64::MAX)) as u64,
    }
}

fn compose_frame(frame: &FrameBuilder) -> Result<Scene, &'static str> {
    finite_transform(frame.header.world_to_surface).ok_or("frame camera is not finite")?;
    let viewport = Rect::new(
        0.0,
        0.0,
        f64::from(frame.header.width),
        f64::from(frame.header.height),
    );
    let mut scene = Scene::new();
    for pass in &frame.passes {
        let grouped = pass.isolated || pass.blend != 0 || pass.opacity < 1.0;
        if grouped {
            scene.push_layer(
                Fill::NonZero,
                blend_mode(pass.blend),
                pass.opacity,
                Affine::IDENTITY,
                &viewport,
            );
        }
        scene.append(&pass.scene, Some(pass.transform));
        if grouped {
            scene.pop_layer();
        }
    }
    Ok(scene)
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

fn render_frame(target: &mut SurfaceTarget, frame: &FrameBuilder) -> Result<(), u8> {
    let width = frame.header.width.max(1);
    let height = frame.header.height.max(1);
    target.resize(width, height);
    let output = acquire_surface_texture(target)?;
    let output_view = output
        .texture
        .create_view(&wgpu::TextureViewDescriptor::default());
    let scene = compose_frame(frame).map_err(|_| FRAME_ERROR)?;
    let render_view = target.render_texture(width, height).clone();
    target
        .renderer
        .render_to_texture(
            &target.device,
            &target.queue,
            &scene,
            &render_view,
            &RenderParams {
                base_color: color(frame.header.background, 1.0),
                width,
                height,
                antialiasing_method: AaConfig::Area,
            },
        )
        .map_err(|_| FRAME_ERROR)?;
    let mut encoder = target
        .device
        .create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("Mapper Vello present"),
        });
    target
        .blitter
        .copy(&target.device, &mut encoder, &render_view, &output_view);
    target.queue.submit(Some(encoder.finish()));
    output.present();
    Ok(())
}

fn render_offscreen(target: &mut OffscreenTarget, frame: &FrameBuilder) -> Result<Vec<u8>, String> {
    let width = frame.header.width.max(1);
    let height = frame.header.height.max(1);
    let scene = compose_frame(frame).map_err(str::to_owned)?;
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
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        let buffer = target.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("Mapper Vello offscreen readback"),
            size: buffer_size,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        target.resources = Some(OffscreenResources {
            texture,
            view,
            buffer,
            width,
            height,
            unpadded_bytes_per_row,
            padded_bytes_per_row,
        });
    }
    let resources = target
        .resources
        .as_ref()
        .expect("offscreen resources exist");
    target
        .renderer
        .render_to_texture(
            &target.device,
            &target.queue,
            &scene,
            &resources.view,
            &RenderParams {
                base_color: color(frame.header.background, 1.0),
                width,
                height,
                antialiasing_method: AaConfig::Area,
            },
        )
        .map_err(|error| format!("Vello offscreen render failed: {error}"))?;
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
    let started = Instant::now();
    if frame.header.surface_sequence != current.sequence {
        publish_result(
            result_tx,
            result_replace_rx,
            frame_result(&frame, FRAME_DROPPED_STALE, 0, started),
        );
        return;
    }
    if current.phase != SURFACE_EXPOSED || current.width == 0 || current.height == 0 {
        publish_result(
            result_tx,
            result_replace_rx,
            frame_result(&frame, FRAME_TARGET_UNAVAILABLE, 0, started),
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
            frame_result(&frame, FRAME_TARGET_UNAVAILABLE, 0, started),
        );
        return;
    }

    let target_ref = target.as_mut().expect("surface target was created");
    let backend = target_ref.backend;
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
    publish_result(
        result_tx,
        result_replace_rx,
        frame_result(&frame, status, backend, started),
    );
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
