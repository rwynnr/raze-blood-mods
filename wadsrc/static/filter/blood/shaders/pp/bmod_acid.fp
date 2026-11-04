// blood2d chaos "Acid trip".
// Breathing: the whole 3D view slowly swells and shrinks (~3.5 s per breath) while ripples roll outward
// from the centre and a melting warp waxes and wanes. Colour sweeps through the hue wheel, blooms
// outward in rings, gets extra saturation, slight fringing and a soft glow on bright areas.
// (World textures also tessellate and bloom; that part is done in the renderer, see hw_walls.cpp.)
// Everything is slow and smooth (all cycles well under 1 Hz); nothing flashes.
// amount (0..1) is faded in and out by bloodmod.cpp; timer is in seconds.

vec3 toYIQ(vec3 c)
{
	return vec3(dot(c, vec3(0.299, 0.587, 0.114)),
	            dot(c, vec3(0.596, -0.274, -0.322)),
	            dot(c, vec3(0.211, -0.523, 0.312)));
}

vec3 fromYIQ(vec3 y)
{
	return vec3(dot(y, vec3(1.0, 0.956, 0.621)),
	            dot(y, vec3(1.0, -0.272, -0.647)),
	            dot(y, vec3(1.0, -1.106, 1.703)));
}

vec3 sampleScene(vec2 uv)
{
	return texture(InputTexture, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
}

void main()
{
	float a = clamp(amount, 0.0, 1.0);
	float t = timer;
	vec2 size = vec2(textureSize(InputTexture, 0));
	float aspect = size.x / max(size.y, 1.0);

	// Centred, aspect-corrected coordinates.
	vec2 p = TexCoord - vec2(0.5);
	p.x *= aspect;
	float r = length(p);
	vec2 dir = r > 0.0001 ? p / r : vec2(0.0);

	// Breathing: a slow swell in and out, plus ripples rolling outward, strongest on the inhale.
	float breath = sin(t * 1.8);
	float inhale = 0.5 + 0.5 * breath;
	vec2 q = p * (1.0 - breath * 0.05 * a);
	q += dir * sin(r * 24.0 - t * 2.4) * (0.004 + 0.006 * inhale) * a;

	// Melting warp that waxes and wanes.
	float melt = (0.5 + 0.5 * sin(t * 0.6)) * a;
	q += vec2(sin(q.y * 7.0 + t * 1.1), sin(q.x * 6.0 - t * 0.9)) * 0.014 * melt;
	q += vec2(sin((q.x + q.y) * 13.0 + t * 0.7), cos((q.x - q.y) * 11.0 - t * 0.5)) * 0.006 * melt;

	vec2 uv = vec2(q.x / aspect, q.y) + vec2(0.5);

	// Colour fringing that breathes too.
	vec2 fringe = vec2((0.002 + 0.003 * inhale) * a, 0.0);
	vec3 c = vec3(sampleScene(uv + fringe).r, sampleScene(uv).g, sampleScene(uv - fringe).b);

	// Soft glow on bright areas.
	vec2 g = vec2(0.006, 0.006 * aspect) * (0.5 + inhale);
	vec3 glow = (sampleScene(uv + vec2(g.x, 0.0)) + sampleScene(uv - vec2(g.x, 0.0))
	           + sampleScene(uv + vec2(0.0, g.y)) + sampleScene(uv - vec2(0.0, g.y))) * 0.25;
	float lum = dot(glow, vec3(0.299, 0.587, 0.114));
	c += glow * smoothstep(0.4, 0.9, lum) * 0.4 * a;

	// Hue sweep (~7 s) with rings of colour blooming outward, plus extra saturation.
	vec3 yiq = toYIQ(c);
	float h = t * 0.9 - r * 4.0 * a;
	float cs = cos(h);
	float sn = sin(h);
	yiq.yz = vec2(yiq.y * cs - yiq.z * sn, yiq.y * sn + yiq.z * cs) * (1.0 + 0.9 * a);
	vec3 trip = clamp(fromYIQ(yiq), 0.0, 1.0);

	vec3 orig = texture(InputTexture, TexCoord).rgb;
	FragColor = vec4(mix(orig, trip, a), 1.0);
}
