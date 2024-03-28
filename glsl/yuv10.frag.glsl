#version 330

uniform sampler2D frame;
uniform ivec2 frame_size;

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

vec3 textureGetYUV(sampler2D sampler, ivec2 px)
{
	int group = px.x / 6;
	int component = px.x % 6;

	ivec2 tx = ivec2(group * 4, px.y);

	ivec2 pos;
	switch(component) {
		case 0:
			pos = ivec2(0, 0);
			break;
		case 1:
			pos = ivec2(0, 1);
			break;
		case 2:
		case 3:
			pos = ivec2(1, 2);
			break;
		case 4:
		case 5:
			pos = ivec2(2, 3);
			break;
	}

	vec4 texel_a = texelFetch(sampler, tx + ivec2(pos.x, 0), 0);
	vec4 texel_b = texelFetch(sampler, tx + ivec2(pos.y, 0), 0);

	switch(component) {
		case 0:
			return vec3(texel_a.g, texel_a.r, texel_a.b);
		case 1:
			return vec3(texel_b.r, texel_a.r, texel_a.b);
		case 2:
			return vec3(texel_a.b, texel_a.g, texel_b.r);
		case 3:
			return vec3(texel_b.g, texel_a.g, texel_b.r);
		case 4:
			return vec3(texel_b.r, texel_a.b, texel_b.g);
		case 5:
			return vec3(texel_b.b, texel_a.b, texel_b.g);
		default:
			return vec3(0.0);
	}
}

void textureGatherYUV(sampler2D sampler, vec2 tc, out vec3 W, out vec3 X, out vec3 Y, out vec3 Z)
{
	ivec2 px = ivec2(tc * frame_size);

	ivec2 tmin = ivec2(0, 0);
	ivec2 tmax = frame_size - ivec2(1, 1);

	W = textureGetYUV(sampler, px);
	X = textureGetYUV(sampler, clamp(px + ivec2(0, 1), tmin, tmax));
	Y = textureGetYUV(sampler, clamp(px + ivec2(1, 1), tmin, tmax));
	Z = textureGetYUV(sampler, clamp(px + ivec2(1, 0), tmin, tmax));
}

void main(void)
{
	vec3 W, X, Y, Z;
	textureGatherYUV(frame, pos, W, X, Y, Z);

	float alpha = 1.0;

	vec4 pixel = rec709YCbCr2rgba(W.r, W.g, W.b, alpha);
	vec4 pixel_u = rec709YCbCr2rgba(X.r, X.g, X.b, alpha);
	vec4 pixel_ur = rec709YCbCr2rgba(Y.r, Y.g, Y.b, alpha);
	vec4 pixel_r = rec709YCbCr2rgba(Z.r, Z.g, Z.b, alpha);

	vec2 off = fract(pos * frame_size);
	color = bilinear(pixel, pixel_u, pixel_ur, pixel_r, off);
}
