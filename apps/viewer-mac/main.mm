// ronviewer: browse and animate Rise of Nations unit graphics on macOS (AppKit + Metal).
//
// Usage: ronviewer [game-data-dir] [--find TEXT] [--anim N] [--time MS]
//                  [--snapshot out.png [--size WxH] [--sheet COLSxROWS] [--yaw R] [--pitch R]]
//   game-data-dir contains Data/unit_graphics.xml, Data/anim_graphics.xml and art/ (defaults to ./gamedata).
//   --snapshot renders offscreen to a PNG and exits (no window). --sheet tiles consecutive units into one image.
//
// Units draw with the game's own render states (platform/metal/renderer.mm): bodies with "Unit Blend", carried props
// with their AMMO render state. The scene renders to an HDR target and is tonemapped, as in the original.
//
// Controls: Left/Right unit (Shift: x20)   Up/Down animation   Space pause   , . step frame
//           drag orbit   scroll/pinch zoom   T team color   C cycle team color   [ ] speed   R reset view   Q quit
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/bh3.hpp"
#include "assets/bha.hpp"
#include "assets/catalog.hpp"
#include "assets/effects.hpp"
#include "assets/pose.hpp"
#include "assets/tga.hpp"
#include "render/lighting.hpp"
#include "render/renderstate.hpp"
#include "renderer.hpp"
#include "unit_draw.hpp"

namespace rm = ron::metal;

// ---------------------------------------------------------------------------------------------- viewer-only shader

// The checkered ground is a viewer aid, not game content, so it has its own trivial shader.
static const char* kGroundShader = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Vertex { packed_float3 position; packed_float3 normal; float2 uv; };
struct Ground { float4x4 view_proj; float cell; float3 pad; };
struct VOut { float4 position [[position]]; float3 world; };
vertex VOut ground_vertex(uint vid [[vertex_id]], const device Vertex* v [[buffer(0)]], constant Ground& g [[buffer(1)]]) {
	VOut o;
	o.world = float3(v[vid].position);
	o.position = g.view_proj * float4(o.world, 1.0);
	return o;
}
fragment float4 ground_fragment(VOut in [[stage_in]], constant Ground& g [[buffer(1)]]) {
	float2 cell = floor(in.world.xy / g.cell);
	float checker = fmod(abs(cell.x + cell.y), 2.0);
	return float4(mix(float3(0.10, 0.12, 0.14), float3(0.14, 0.16, 0.19), checker), 1.0);
}
)METAL";

struct GroundUniforms {
	simd_float4x4 view_proj;
	float cell;
	float pad[3];
};


static const MTLPixelFormat kOutputFormat = MTLPixelFormatBGRA8Unorm;

static simd_float4x4 perspective(float fovy, float aspect, float zn, float zf) {
	const float ys = 1.0f / std::tan(fovy * 0.5f), xs = ys / aspect, zs = zf / (zn - zf);
	return simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
	                   simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * zn, 0));
}

static simd_float4x4 look_at(simd_float3 eye, simd_float3 target, simd_float3 up) {
	const simd_float3 z = simd_normalize(eye - target);
	const simd_float3 x = simd_normalize(simd_cross(up, z));
	const simd_float3 y = simd_cross(z, x);
	return simd_matrix(simd_make_float4(x.x, y.x, z.x, 0), simd_make_float4(x.y, y.y, z.y, 0),
	                   simd_make_float4(x.z, y.z, z.z, 0),
	                   simd_make_float4(-simd_dot(x, eye), -simd_dot(y, eye), -simd_dot(z, eye), 1));
}

static const simd_float3 kTeamColors[] = {
	{0.15f, 0.35f, 0.95f}, {0.90f, 0.15f, 0.12f}, {0.20f, 0.75f, 0.25f}, {0.95f, 0.85f, 0.15f},
	{0.60f, 0.25f, 0.80f}, {0.10f, 0.80f, 0.85f}, {0.95f, 0.50f, 0.10f}, {0.95f, 0.95f, 0.95f},
};
static const int kTeamColorCount = int(sizeof(kTeamColors) / sizeof(kTeamColors[0]));

// ---------------------------------------------------------------------------------------------- renderer

@interface Renderer : NSObject <MTKViewDelegate>
@property(nonatomic, weak) MTKView* view;  // optional; receives the title
- (instancetype)initWithDevice:(id<MTLDevice>)device
                       gameDir:(NSString*)dir
                          find:(NSString*)find
                         error:(NSString* __autoreleasing*)error;
- (void)selectAnim:(int)anim timeMs:(double)ms;
- (void)setYaw:(float)yaw pitch:(float)pitch;
- (BOOL)snapshotTo:(NSString*)path width:(int)w height:(int)h cols:(int)cols rows:(int)rows;
- (void)handleKey:(NSEvent*)event;
- (void)orbitBy:(CGFloat)dx dy:(CGFloat)dy;
- (void)zoomBy:(CGFloat)amount;
@end

@implementation Renderer {
	id<MTLDevice> _device;
	id<MTLCommandQueue> _queue;
	rm::StateCache _states;
	rm::PostProcess _post;
	rm::SceneTargets _targets;
	rm::UnitDrawer _drawer;
	ron::SceneLighting _lighting;
	std::unique_ptr<ron::GameFiles> _files;
	id<MTLRenderPipelineState> _groundPipeline;
	id<MTLDepthStencilState> _groundDepth;

	int _unit;
	int _anim;
	std::vector<ron::PosedVertex> _posed;
	ron::Vec3 _center;
	float _radius;
	float _groundZ;

	double _timeMs;
	double _lastFrame;
	double _lastTitle;
	float _speed;
	bool _paused;
	bool _team;
	int _teamIndex;
	float _yaw, _pitch, _zoom;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device
                       gameDir:(NSString*)dir
                          find:(NSString*)find
                         error:(NSString* __autoreleasing*)error {
	if (!(self = [super init])) return nil;
	_device = device;
	_queue = [_device newCommandQueue];
	_speed = 1.0f;
	_team = true;
	_radius = 1.0f;
	[self resetCamera];

	std::string err;
	if (!_states.init(_device, rm::SceneTargets::kColor, rm::SceneTargets::kDepth, err) ||
	    !_post.init(_device, _states.library(), kOutputFormat, err)) {
		*error = @(err.c_str());
		return nil;
	}
	NSError* nserr = nil;
	id<MTLLibrary> lib = [_device newLibraryWithSource:@(kGroundShader) options:nil error:&nserr];
	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"ground_vertex"];
	pd.fragmentFunction = [lib newFunctionWithName:@"ground_fragment"];
	pd.colorAttachments[0].pixelFormat = rm::SceneTargets::kColor;
	pd.depthAttachmentPixelFormat = rm::SceneTargets::kDepth;
	_groundPipeline = lib ? [_device newRenderPipelineStateWithDescriptor:pd error:&nserr] : nil;
	if (!_groundPipeline) {
		*error = [NSString stringWithFormat:@"Ground pipeline failed: %@", nserr.localizedDescription];
		return nil;
	}
	MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
	dd.depthCompareFunction = MTLCompareFunctionLess;
	dd.depthWriteEnabled = YES;
	_groundDepth = [_device newDepthStencilStateWithDescriptor:dd];

	try {
		_files = std::make_unique<ron::GameFiles>(dir.UTF8String);
		_drawer.init(_device, _queue, &_states, _files.get());
		_lighting = ron::load_scene_lighting(*_files);
		const float* a = _lighting.unit_ambient;
		const float* d = _lighting.unit_diffuse;
		_drawer.set_unit_material(simd_make_float4(a[0], a[1], a[2], 1), simd_make_float4(d[0], d[1], d[2], 1));
	} catch (const std::exception& e) {
		*error = [NSString stringWithFormat:@"Could not load game data from %@:\n%s", dir, e.what()];
		return nil;
	}
	const auto& units = _drawer.units();
	if (units.empty()) {
		*error = [NSString stringWithFormat:@"No units found in %@/Data/unit_graphics.xml", dir];
		return nil;
	}
	std::printf("Loaded %zu unit graphics from %s\n", units.size(), dir.UTF8String);

	int start = 0;
	if (find.length) {
		std::string needle = find.lowercaseString.UTF8String;
		for (size_t i = 0; i < units.size(); i++) {
			if (ron::to_lower(units[i].name + " " + units[i].model).find(needle) != std::string::npos) {
				start = int(i);
				break;
			}
		}
	}
	[self loadUnit:start];
	_lastFrame = CACurrentMediaTime();
	return self;
}

- (void)resetCamera {
	_yaw = 0.6f;
	_pitch = 0.35f;
	_zoom = 1.0f;
}

- (void)setYaw:(float)yaw pitch:(float)pitch {
	_yaw = yaw;
	_pitch = pitch;
}

- (void)loadUnit:(int)index {
	const int n = int(_drawer.units().size());
	_unit = ((index % n) + n) % n;
	[self loadAnim:0];
	[self frameModel];
}

- (void)loadAnim:(int)index {
	_timeMs = 0;
	const int n = int(_drawer.units()[size_t(_unit)].anims.size());
	_anim = n ? ((index % n) + n) % n : -1;
}

- (void)selectAnim:(int)anim timeMs:(double)ms {
	[self loadAnim:anim];
	_timeMs = ms;
	_paused = true;
}

- (void)frameModel {
	if (!_drawer.pose(_unit, _anim, 0, _posed)) return;
	ron::bounding_sphere(_posed, _center, _radius);
	if (_radius < 1e-3f) _radius = 1.0f;
	float minZ = 0;
	for (const auto& v : _posed) minZ = std::min(minZ, v.pz);
	_groundZ = minZ;
}

- (void)handleKey:(NSEvent*)event {
	NSString* chars = event.charactersIgnoringModifiers;
	const unichar c = chars.length ? [chars characterAtIndex:0] : 0;
	const bool shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
	switch (c) {
		case NSRightArrowFunctionKey: [self loadUnit:_unit + (shift ? 20 : 1)]; break;
		case NSLeftArrowFunctionKey: [self loadUnit:_unit - (shift ? 20 : 1)]; break;
		case NSDownArrowFunctionKey: [self loadAnim:_anim + 1]; break;
		case NSUpArrowFunctionKey: [self loadAnim:_anim - 1]; break;
		case ' ': _paused = !_paused; break;
		case ',': _paused = true; _timeMs = std::max(0.0, _timeMs - 1000.0 / 30.0); break;
		case '.': _paused = true; _timeMs += 1000.0 / 30.0; break;
		case '[': _speed = std::max(0.05f, _speed / 1.5f); break;
		case ']': _speed = std::min(20.0f, _speed * 1.5f); break;
		case 't': case 'T': _team = !_team; break;
		case 'c': case 'C': _teamIndex = (_teamIndex + 1) % kTeamColorCount; break;
		case 'r': case 'R': [self resetCamera]; break;
		case 'q': case 'Q': [NSApp terminate:nil]; break;
		default: break;
	}
	_lastTitle = 0;  // refresh the title now
}

- (void)orbitBy:(CGFloat)dx dy:(CGFloat)dy {
	_yaw -= float(dx) * 0.01f;
	_pitch = std::clamp(_pitch + float(dy) * 0.01f, -1.5f, 1.5f);
}

- (void)zoomBy:(CGFloat)amount {
	_zoom = std::clamp(_zoom * std::exp(-float(amount)), 0.1f, 20.0f);
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)updateTitle {
	const ron::UnitGraphic& u = _drawer.units()[size_t(_unit)];
	std::string anim = "(no animation)";
	const uint32_t clip = _drawer.clip_ms(_unit, _anim);
	if (_anim >= 0) {
		char buf[256];
		std::snprintf(buf, sizeof buf, "%s (%d/%zu)%s", u.anims[size_t(_anim)].action.c_str(), _anim + 1, u.anims.size(),
		              clip ? "" : " [file missing]");
		anim = buf;
	}
	const double t = clip ? std::fmod(_timeMs, double(clip)) / 1000.0 : 0.0;
	const std::string status = _drawer.status(_unit);
	char title[768];
	std::snprintf(title, sizeof title, "%d/%zu  %s  —  %s  %.2f/%.2fs  x%.2f%s%s%s%s", _unit + 1, _drawer.units().size(),
	              u.name.c_str(), anim.c_str(), t, clip / 1000.0, _speed, _paused ? "  [paused]" : "", _team ? "  [team]" : "",
	              status.empty() ? "" : "  ⚠ ", status.c_str());
	self.view.window.title = @(title);
}

// Draws the current unit into the encoder's current viewport, of `size` pixels.
- (void)encodeScene:(id<MTLRenderCommandEncoder>)enc size:(CGSize)size {
	const float aspect = size.height > 0 ? float(size.width / size.height) : 1.0f;
	const float dist = _radius * 2.8f * _zoom;
	const simd_float3 target = simd_make_float3(_center.x, _center.y, _center.z);
	const simd_float3 eye = target + dist * simd_make_float3(std::cos(_pitch) * std::sin(_yaw),
	                                                         -std::cos(_pitch) * std::cos(_yaw), std::sin(_pitch));
	rm::SceneUniforms scene{};
	scene.view = look_at(eye, target, simd_make_float3(0, 0, 1));
	scene.proj = perspective(0.7f, aspect, dist * 0.01f, dist * 20.0f);
	// The object pass: the Sun at full white, the scene ambient from <UNITS> (docs/lighting-spec.md section 3).
	scene.light_dir = simd_make_float4(ron::kSunDir[0], ron::kSunDir[1], ron::kSunDir[2], 0);
	scene.light_diffuse = simd_make_float4(1, 1, 1, 1);
	scene.ambient_light = simd_make_float4(_lighting.ambient_light[0], _lighting.ambient_light[1], _lighting.ambient_light[2], 1);

	// Ground: a checkered quad under the model, for orientation.
	const float g = _radius * 4.0f, z = _groundZ;
	const ron::PosedVertex quad[6] = {
		{_center.x - g, _center.y - g, z, 0, 0, 1, 0, 0}, {_center.x + g, _center.y - g, z, 0, 0, 1, 0, 0},
		{_center.x + g, _center.y + g, z, 0, 0, 1, 0, 0}, {_center.x - g, _center.y - g, z, 0, 0, 1, 0, 0},
		{_center.x + g, _center.y + g, z, 0, 0, 1, 0, 0}, {_center.x - g, _center.y + g, z, 0, 0, 1, 0, 0},
	};
	GroundUniforms gu{simd_mul(scene.proj, scene.view), std::max(_radius / 4.0f, 1.0f), {}};
	[enc setRenderPipelineState:_groundPipeline];
	[enc setDepthStencilState:_groundDepth];
	[enc setCullMode:MTLCullModeNone];
	[enc setVertexBytes:quad length:sizeof quad atIndex:0];
	[enc setVertexBytes:&gu length:sizeof gu atIndex:1];
	[enc setFragmentBytes:&gu length:sizeof gu atIndex:1];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];

	[enc setVertexBytes:&scene length:sizeof scene atIndex:2];
	[enc setFragmentBytes:&scene length:sizeof scene atIndex:2];
	const simd_float4 team = _team ? simd_make_float4(kTeamColors[_teamIndex], 1) : simd_make_float4(1, 1, 1, 1);
	_drawer.draw(enc, _unit, _anim, _timeMs, matrix_identity_float4x4, team);
}

// Renders the scene (one cell per unit for a sheet) into the HDR target, then tonemaps into `output`.
// Returns a label line per cell.
- (std::string)renderInto:(id<MTLTexture>)output commandBuffer:(id<MTLCommandBuffer>)cb cols:(int)cols rows:(int)rows {
	const NSUInteger w = output.width, h = output.height;
	_targets.ensure(_device, w, h);
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:_targets.scene_pass(MTLClearColorMake(0.035, 0.04, 0.05, 1.0))];
	std::string labels;
	const int first = _unit, anim = std::max(_anim, 0);
	const double time = _timeMs;
	const double cw = double(w) / cols, ch = double(h) / rows;
	for (int i = 0; i < cols * rows; i++) {
		if (i > 0) {
			[self loadUnit:first + i];
			[self selectAnim:anim timeMs:time];
		}
		const double x = (i % cols) * cw, y = (i / cols) * ch;
		[enc setViewport:(MTLViewport){x, y, cw, ch, 0.0, 1.0}];
		[enc setScissorRect:(MTLScissorRect){NSUInteger(x), NSUInteger(y), NSUInteger(cw), NSUInteger(ch)}];
		[self encodeScene:enc size:CGSizeMake(cw, ch)];
		const ron::UnitGraphic& u = _drawer.units()[size_t(_unit)];
		const std::string status = _drawer.status(_unit);
		labels += std::to_string(i) + "\t" + std::to_string(_unit) + "\t" + u.name +
		          (_anim >= 0 ? "\t" + u.anims[size_t(_anim)].action : std::string("\t-")) +
		          (status.empty() ? "" : "\t" + status) + "\n";
	}
	[enc endEncoding];

	_post.encode(cb, _targets.hdr(), output, rm::tonemap_params(ron::ToneMapSettings{}), true);
	return labels;
}

- (void)drawInMTKView:(MTKView*)view {
	const double now = CACurrentMediaTime();
	const double dt = std::min(0.1, now - _lastFrame);
	_lastFrame = now;
	if (!_paused) _timeMs += dt * 1000.0 * _speed;
	if (now - _lastTitle > 0.2) {
		_lastTitle = now;
		[self updateTitle];
	}
	id<CAMetalDrawable> drawable = view.currentDrawable;
	if (!drawable) return;
	id<MTLCommandBuffer> cb = [_queue commandBuffer];
	[self renderInto:drawable.texture commandBuffer:cb cols:1 rows:1];
	[cb presentDrawable:drawable];
	[cb commit];
}

// Offscreen render to a PNG. With cols x rows > 1, consecutive units from the current one fill a grid of cells.
- (BOOL)snapshotTo:(NSString*)path width:(int)w height:(int)h cols:(int)cols rows:(int)rows {
	id<MTLTexture> color = rm::make_readback_target(_device, w, h);
	id<MTLCommandBuffer> cb = [_queue commandBuffer];
	const std::string labels = [self renderInto:color commandBuffer:cb cols:cols rows:rows];
	[cb commit];
	[cb waitUntilCompleted];
	if (!rm::write_png(color, path.UTF8String)) {
		std::fprintf(stderr, "could not write %s\n", path.UTF8String);
		return NO;
	}
	std::printf("cell\tunit\tname\tanim\tstatus\n%s", labels.c_str());
	std::printf("wrote %s (%dx%d)\n", path.UTF8String, w, h);
	return YES;
}

@end

// ---------------------------------------------------------------------------------------------- view + app

@interface ViewerView : MTKView
@property(nonatomic, weak) Renderer* renderer;
@end

@implementation ViewerView
- (BOOL)acceptsFirstResponder {
	return YES;
}
- (void)keyDown:(NSEvent*)event {
	[self.renderer handleKey:event];
}
- (void)mouseDragged:(NSEvent*)event {
	[self.renderer orbitBy:event.deltaX dy:event.deltaY];
}
- (void)scrollWheel:(NSEvent*)event {
	[self.renderer zoomBy:event.scrollingDeltaY * (event.hasPreciseScrollingDeltas ? 0.01 : 0.1)];
}
- (void)magnifyWithEvent:(NSEvent*)event {
	[self.renderer zoomBy:event.magnification];
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) Renderer* renderer;
@property(nonatomic, copy) NSString* gameDir;
@property(nonatomic, copy) NSString* find;
@property(nonatomic) int anim;
@property(nonatomic) double timeMs;
@end

@implementation AppDelegate

- (void)fail:(NSString*)message {
	std::fprintf(stderr, "%s\n", message.UTF8String);
	NSAlert* alert = [NSAlert new];
	alert.messageText = @"RoN viewer";
	alert.informativeText = message;
	[alert runModal];
	[NSApp terminate:nil];
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device) {
		[self fail:@"This Mac has no Metal device."];
		return;
	}
	const NSRect frame = NSMakeRect(0, 0, 1280, 800);
	self.window = [[NSWindow alloc] initWithContentRect:frame
	                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                                                    NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
	                                            backing:NSBackingStoreBuffered
	                                              defer:NO];
	ViewerView* view = [[ViewerView alloc] initWithFrame:frame device:device];
	view.colorPixelFormat = kOutputFormat;
	view.framebufferOnly = YES;
	view.preferredFramesPerSecond = 60;

	NSString* error = nil;
	self.renderer = [[Renderer alloc] initWithDevice:device gameDir:self.gameDir find:self.find error:&error];
	if (!self.renderer) {
		[self fail:error ?: @"Renderer initialization failed."];
		return;
	}
	if (self.anim >= 0) [self.renderer selectAnim:self.anim timeMs:self.timeMs];
	self.renderer.view = view;
	view.delegate = self.renderer;
	view.renderer = self.renderer;
	self.window.contentView = view;
	[self.window center];
	[self.window makeKeyAndOrderFront:nil];
	[self.window makeFirstResponder:view];
	[NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
	return YES;
}

@end

static NSString* default_game_dir() {
	NSFileManager* fm = [NSFileManager defaultManager];
	NSString* exeDir = [NSBundle.mainBundle.executablePath stringByDeletingLastPathComponent];
	NSArray<NSString*>* candidates = @[
		[fm.currentDirectoryPath stringByAppendingPathComponent:@"gamedata"],
		[exeDir stringByAppendingPathComponent:@"gamedata"],
		[[exeDir stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"gamedata"],
		[NSHomeDirectory() stringByAppendingPathComponent:@"Games/RiseOfNations"],
	];
	for (NSString* c in candidates) {
		BOOL isDir = NO;
		if ([fm fileExistsAtPath:c isDirectory:&isDir] && isDir) return c;
	}
	return candidates[0];
}

int main(int argc, const char* argv[]) {
	@autoreleasepool {
		NSString* gameDir = nil;
		NSString* find = nil;
		NSString* snapshot = nil;
		int anim = -1, w = 1024, h = 1024, cols = 1, rows = 1;
		double timeMs = 0;
		float yaw = 0.6f, pitch = 0.35f;
		for (int i = 1; i < argc; i++) {
			const std::string a = argv[i];
			const bool more = i + 1 < argc;
			if (a == "--find" && more) {
				find = @(argv[++i]);
			} else if (a == "--anim" && more) {
				anim = std::atoi(argv[++i]);
			} else if (a == "--time" && more) {
				timeMs = std::atof(argv[++i]);
			} else if (a == "--snapshot" && more) {
				snapshot = @(argv[++i]);
			} else if (a == "--size" && more) {
				std::sscanf(argv[++i], "%dx%d", &w, &h);
			} else if (a == "--sheet" && more) {
				std::sscanf(argv[++i], "%dx%d", &cols, &rows);
			} else if (a == "--yaw" && more) {
				yaw = float(std::atof(argv[++i]));
			} else if (a == "--pitch" && more) {
				pitch = float(std::atof(argv[++i]));
			} else if (a == "--help" || a == "-h" || a.rfind("--", 0) == 0) {
				std::printf("usage: %s [game-data-dir] [--find TEXT] [--anim N] [--time MS]\n"
				            "          [--snapshot out.png [--size WxH] [--sheet COLSxROWS] [--yaw R] [--pitch R]]\n",
				            argv[0]);
				return a == "--help" || a == "-h" ? 0 : 2;
			} else {
				gameDir = @(argv[i]);
			}
		}
		if (snapshot) {
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			if (!device) {
				std::fprintf(stderr, "no Metal device\n");
				return 1;
			}
			NSString* error = nil;
			Renderer* r = [[Renderer alloc] initWithDevice:device gameDir:gameDir ?: default_game_dir() find:find error:&error];
			if (!r) {
				std::fprintf(stderr, "%s\n", error.UTF8String);
				return 1;
			}
			[r selectAnim:std::max(anim, 0) timeMs:timeMs];
			[r setYaw:yaw pitch:pitch];
			return [r snapshotTo:snapshot width:w height:h cols:std::max(cols, 1) rows:std::max(rows, 1)] ? 0 : 1;
		}
		std::printf(
			"Controls: Left/Right unit (Shift x20) | Up/Down animation | Space pause | , . step | drag orbit |\n"
			"          scroll/pinch zoom | T team color | C cycle color | [ ] speed | R reset view | Q quit\n");

		NSApplication* app = [NSApplication sharedApplication];
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];

		NSMenu* menuBar = [NSMenu new];
		NSMenuItem* appItem = [NSMenuItem new];
		[menuBar addItem:appItem];
		NSMenu* appMenu = [NSMenu new];
		[appMenu addItemWithTitle:@"Quit RoN Viewer" action:@selector(terminate:) keyEquivalent:@"q"];
		appItem.submenu = appMenu;
		app.mainMenu = menuBar;

		AppDelegate* delegate = [AppDelegate new];
		delegate.gameDir = gameDir ?: default_game_dir();
		delegate.find = find;
		delegate.anim = anim;
		delegate.timeMs = timeMs;
		app.delegate = delegate;
		[app run];
	}
	return 0;
}
