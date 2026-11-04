// blood2d chaos "Acid trip": slow wave distortion, a hue sweep, boosted saturation and slight
// colour fringing. Everything changes smoothly (the hue cycle takes ~7 s); nothing flashes.
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

void main()
{
	float a = clamp(amount, 0.0, 1.0);
	float t = timer;

	vec2 uv = TexCoord;
	uv.x += sin(uv.y * 8.0 + t * 1.1) * 0.010 * a;
	uv.y += sin(uv.x * 6.0 + t * 0.8) * 0.008 * a;
	uv = clamp(uv, vec2(0.0), vec2(1.0));

	vec2 fringe = vec2(0.003 * a, 0.0);
	vec3 c;
	c.r = texture(InputTexture, clamp(uv + fringe, vec2(0.0), vec2(1.0))).r;
	c.g = texture(InputTexture, uv).g;
	c.b = texture(InputTexture, clamp(uv - fringe, vec2(0.0), vec2(1.0))).b;

	vec3 yiq = toYIQ(c);
	float h = t * 0.9;
	float cs = cos(h);
	float sn = sin(h);
	yiq.yz = vec2(yiq.y * cs - yiq.z * sn, yiq.y * sn + yiq.z * cs) * (1.0 + 0.9 * a);
	vec3 trip = clamp(fromYIQ(yiq), 0.0, 1.0);

	vec3 orig = texture(InputTexture, TexCoord).rgb;
	FragColor = vec4(mix(orig, trip, a), 1.0);
}
