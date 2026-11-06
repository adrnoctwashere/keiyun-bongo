/*
 * Noctis Bongo - native OBS source plugin (Windows).
 * Draws your image and animates a mouse hand + keyboard paw:
 *   - paw presses while any "reactive key" (default WASD) is held
 *   - hand slides with mouse movement
 * Input is polled with GetAsyncKeyState / GetCursorPos, so it works while OBS is unfocused.
 */
#include <obs-module.h>
#include <graphics/image-file.h>
#include <plugin-support.h>
#include <windows.h>
#include <ctype.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static const char *EFFECT =
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d image;\n"
	"uniform float4 mask;\n"
	"sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
	"struct VertInOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
	"VertInOut VSMain(VertInOut v) {\n"
	"  VertInOut o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
	"float4 PSMain(VertInOut v) : TARGET {\n"
	"  float4 c = image.Sample(textureSampler, v.uv);\n"
	"  float2 d = (v.uv - mask.xy) / mask.zw;\n"
	"  return c * (1.0 - smoothstep(0.8, 1.0, length(d))); }\n"
	"technique Draw { pass { vertex_shader = VSMain(v); pixel_shader = PSMain(v); } }\n";

struct bongo {
	obs_source_t *source;
	gs_effect_t *fx;
	gs_image_file_t img;
	bool loaded;
	char keys[16];
	bool mouse;
	float hand[2], paw[2]; /* centers of the moving cutouts, 0..1 of the image */
	POINT last;
	bool have_last;
	float pawAmt, handAmt, ox, oy, tx, ty, still;
};

static float clampf(float v, float m) { return v > m ? m : (v < -m ? -m : v); }
static float approach(float v, float target, float step)
{
	if (step > 1.f) step = 1.f;
	return v + (target - v) * step;
}

static const char *bongo_name(void *unused) { UNUSED_PARAMETER(unused); return "Noctis Bongo"; }

static void unload(struct bongo *b)
{
	if (!b->loaded) return;
	obs_enter_graphics();
	gs_image_file_free(&b->img);
	obs_leave_graphics();
	b->loaded = false;
}

static void bongo_update(void *data, obs_data_t *s)
{
	struct bongo *b = data;
	unload(b);
	const char *path = obs_data_get_string(s, "file");
	if (path && *path) {
		gs_image_file_init(&b->img, path);
		obs_enter_graphics();
		gs_image_file_init_texture(&b->img);
		obs_leave_graphics();
		b->loaded = b->img.loaded;
	}
	strncpy(b->keys, obs_data_get_string(s, "keys"), sizeof(b->keys) - 1);
	b->mouse = obs_data_get_bool(s, "mouse");
	b->hand[0] = (float)obs_data_get_double(s, "hand_x") / 100.f;
	b->hand[1] = (float)obs_data_get_double(s, "hand_y") / 100.f;
	b->paw[0] = (float)obs_data_get_double(s, "paw_x") / 100.f;
	b->paw[1] = (float)obs_data_get_double(s, "paw_y") / 100.f;
}

static void *bongo_create(obs_data_t *settings, obs_source_t *source)
{
	struct bongo *b = bzalloc(sizeof(*b));
	b->source = source;
	char *err = NULL;
	obs_enter_graphics();
	b->fx = gs_effect_create(EFFECT, "noctis_mask", &err);
	obs_leave_graphics();
	bfree(err);
	bongo_update(b, settings);
	return b;
}

static void bongo_destroy(void *data)
{
	struct bongo *b = data;
	unload(b);
	obs_enter_graphics();
	if (b->fx) gs_effect_destroy(b->fx);
	obs_leave_graphics();
	bfree(b);
}

static void bongo_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "keys", "wasd");
	obs_data_set_default_bool(s, "mouse", true);
	obs_data_set_default_double(s, "hand_x", 26.0);
	obs_data_set_default_double(s, "hand_y", 64.2);
	obs_data_set_default_double(s, "paw_x", 69.4);
	obs_data_set_default_double(s, "paw_y", 59.5);
}

static obs_properties_t *bongo_props(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_path(p, "file", "Image (PNG)", OBS_PATH_FILE, "Images (*.png)", NULL);
	obs_properties_add_text(p, "keys", "Reactive keys", OBS_TEXT_DEFAULT);
	obs_properties_add_bool(p, "mouse", "React to mouse movement");
	obs_properties_add_float_slider(p, "hand_x", "Mouse hand X %", 0, 100, 0.1);
	obs_properties_add_float_slider(p, "hand_y", "Mouse hand Y %", 0, 100, 0.1);
	obs_properties_add_float_slider(p, "paw_x", "Keyboard paw X %", 0, 100, 0.1);
	obs_properties_add_float_slider(p, "paw_y", "Keyboard paw Y %", 0, 100, 0.1);
	return p;
}

static void bongo_tick(void *data, float t)
{
	struct bongo *b = data;
	bool down = false;
	for (const char *k = b->keys; *k; k++)
		if (GetAsyncKeyState(toupper((unsigned char)*k)) & 0x8000) down = true;
	b->pawAmt = approach(b->pawAmt, down ? 1.f : 0.f, t / 0.07f);

	if (b->mouse) {
		POINT p;
		if (GetCursorPos(&p)) {
			if (b->have_last && (p.x != b->last.x || p.y != b->last.y)) {
				b->tx = clampf((p.x - b->last.x) * 0.6f, 9.f);
				b->ty = clampf((p.y - b->last.y) * 0.6f, 9.f);
				b->still = 0.11f;
			}
			b->last = p;
			b->have_last = true;
		}
	}
	if (b->still > 0.f) b->still -= t;
	else b->tx = b->ty = 0.f;
	b->ox = approach(b->ox, b->tx, t / 0.07f);
	b->oy = approach(b->oy, b->ty, t / 0.07f);
	b->handAmt = approach(b->handAmt, b->still > 0.f ? 1.f : 0.f, t / 0.07f);
}

/* draw the whole image again, masked to a soft ellipse, scaled/moved about its center */
static void part(gs_effect_t *fx, gs_texture_t *tex, uint32_t w, uint32_t h, float cx, float cy, float rx, float ry,
		 float ox, float oy, float s)
{
	struct vec4 m;
	vec4_set(&m, cx, cy, rx, ry);
	gs_effect_set_vec4(gs_effect_get_param_by_name(fx, "mask"), &m);
	gs_matrix_push();
	gs_matrix_translate3f(cx * w + ox, cy * h + oy, 0.f);
	gs_matrix_scale3f(s, s, 1.f);
	gs_matrix_translate3f(-cx * w, -cy * h, 0.f);
	while (gs_effect_loop(fx, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
	gs_matrix_pop();
}

static void bongo_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct bongo *b = data;
	if (!b->loaded || !b->img.texture) return;
	gs_texture_t *tex = b->img.texture;
	uint32_t w = b->img.cx, h = b->img.cy;
	float sc = w / 828.f;

	const bool prev = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(def, "image"), tex);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, w, h);

	if (b->fx) {
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(b->fx, "image"), tex);
		/* scaling up slightly keeps the original hand/paw fully covered while they move */
		if (b->handAmt > 0.01f)
			part(b->fx, tex, w, h, b->hand[0], b->hand[1], 0.106f, 0.147f, b->ox * sc * b->handAmt,
			     b->oy * sc * b->handAmt, 1.f + 0.07f * b->handAmt);
		if (b->pawAmt > 0.01f)
			part(b->fx, tex, w, h, b->paw[0], b->paw[1], 0.063f, 0.105f, -4.f * sc * b->pawAmt,
			     8.f * sc * b->pawAmt, 1.f + 0.10f * b->pawAmt);
	}
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev);
}

static uint32_t bongo_w(void *data) { struct bongo *b = data; return b->loaded ? b->img.cx : 0; }
static uint32_t bongo_h(void *data) { struct bongo *b = data; return b->loaded ? b->img.cy : 0; }

static struct obs_source_info bongo_info = {
	.id = "noctis_bongo",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = bongo_name,
	.create = bongo_create,
	.destroy = bongo_destroy,
	.update = bongo_update,
	.get_defaults = bongo_defaults,
	.get_properties = bongo_props,
	.video_tick = bongo_tick,
	.video_render = bongo_render,
	.get_width = bongo_w,
	.get_height = bongo_h,
};

bool obs_module_load(void)
{
	obs_register_source(&bongo_info);
	obs_log(LOG_INFO, "Noctis Bongo loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void) {}
