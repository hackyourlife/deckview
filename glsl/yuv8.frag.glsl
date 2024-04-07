#version 330

uniform sampler2D frame;
uniform bool interpolate = false;
uniform float brightness = 1.0;

in  vec2 pos;
out vec4 color;

vec4 rec709YCbCr2rgba(float Y, float Cb, float Cr, float a)
{
	float r, g, b;

	// Y: Undo 1/256 texture value scaling and scale [16..235] to [0..1] range
	// C: Undo 1/256 texture value scaling and scale [16..240] to [-0.5 .. + 0.5] range
	Y = (Y * 256.0 - 16.0) / 219.0;
	Cb = (Cb * 256.0 - 16.0) / 224.0 - 0.5;
	Cr = (Cr * 256.0 - 16.0) / 224.0 - 0.5;

	// Convert to RGB using Rec.709 conversion matrix (see eq 26.7 in Poynton 2003)
	r = Y + 1.5748 * Cr;
	g = Y - 0.1873 * Cb - 0.4681 * Cr;
	b = Y + 1.8556 * Cb;

	return vec4(r, g, b, a);
}

// Perform bilinear interpolation between the provided components.
// The samples are expected as shown:
// +-------+
// | X | Y |
// |---+---|
// | W | Z |
// +-------+
vec4 bilinear(vec4 W, vec4 X, vec4 Y, vec4 Z, vec2 weight)
{
	vec4 m0 = mix(W, Z, weight.x);
	vec4 m1 = mix(X, Y, weight.x);
	return mix(m0, m1, weight.y);
}

// Gather neighboring YUV macropixels from the given texture coordinate
void textureGatherYUV(sampler2D sampler, vec2 tc, out vec4 W, out vec4 X, out vec4 Y, out vec4 Z)
{
	ivec2 tx = ivec2(tc * textureSize(sampler, 0));
	ivec2 tmin = ivec2(0, 0);
	ivec2 tmax = textureSize(sampler, 0) - ivec2(1, 1);
	W = texelFetch(sampler, tx, 0);
	X = texelFetch(sampler, clamp(tx + ivec2(0, 1), tmin, tmax), 0);
	Y = texelFetch(sampler, clamp(tx + ivec2(1, 1), tmin, tmax), 0);
	Z = texelFetch(sampler, clamp(tx + ivec2(1, 0), tmin, tmax), 0);
}

vec4 color_control(vec4 pixel, float brightness)
{
	vec3 scaled = pixel.rgb * vec3(brightness);
	vec3 clamped = clamp(scaled, vec3(0.0), vec3(1.0));
	return vec4(clamped, pixel.a);
}

void main(void)
{
	/* The shader uses texelFetch to obtain the YUV macropixels to avoid
	 * unwanted interpolation introduced by the GPU interpreting the YUV
	 * data as RGBA pixels. The YUV macropixels are converted into
	 * individual RGB pixels and bilinear interpolation is applied. */
	float alpha = 1.0;

	vec4 macro, macro_u, macro_r, macro_ur;
	vec4 pixel, pixel_r, pixel_u, pixel_ur;

	textureGatherYUV(frame, pos, macro, macro_u, macro_ur, macro_r);

	// Select the components for the bilinear interpolation based on the
	// texture coordinate location within the YUV macropixel:
	// +---------------+          +--------------------+
	// | UY/VY | UY/VY |          | macro_u | macro_ur |
	// |-------|-------|    =>    |---------|----------|
	// | UY/VY | UY/VY |          | macro   | macro_r  |
	// |-------|-------|          +--------------------+
	// | RG/BA | RG/BA |
	// +---------------+
	vec2 off = fract(pos * textureSize(frame, 0));
	if(off.x > 0.5) { // right half of macropixel
		pixel = color_control(rec709YCbCr2rgba(macro.a, macro.b, macro.r, alpha), brightness);
		pixel_r = color_control(rec709YCbCr2rgba(macro_r.g, macro_r.b, macro_r.r, alpha), brightness);
		pixel_u = color_control(rec709YCbCr2rgba(macro_u.a, macro_u.b, macro_u.r, alpha), brightness);
		pixel_ur = color_control(rec709YCbCr2rgba(macro_ur.g, macro_ur.b, macro_ur.r, alpha), brightness);
	} else { // left half & center of macropixel
		pixel = color_control(rec709YCbCr2rgba(macro.g, macro.b, macro.r, alpha), brightness);
		pixel_r = color_control(rec709YCbCr2rgba(macro.a, macro.b, macro.r, alpha), brightness);
		pixel_u = color_control(rec709YCbCr2rgba(macro_u.g, macro_u.b, macro_u.r, alpha), brightness);
		pixel_ur = color_control(rec709YCbCr2rgba(macro_u.a, macro_u.b, macro_u.r, alpha), brightness);
	}

	if(interpolate) {
		color = bilinear(pixel, pixel_u, pixel_ur, pixel_r, off);
	} else {
		color = pixel;
	}
}
