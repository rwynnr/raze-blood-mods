// blood2d chaos "Upside down" (flipy) and "Mirror world" (flipx): flips the 3D view only.

void main()
{
	vec2 uv = TexCoord;
	if (flipx > 0.5) uv.x = 1.0 - uv.x;
	if (flipy > 0.5) uv.y = 1.0 - uv.y;
	FragColor = vec4(texture(InputTexture, uv).rgb, 1.0);
}
