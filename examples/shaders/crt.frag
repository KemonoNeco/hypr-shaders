// Example user shader for hypr-shaders: scanlines + RGB split + slow roll bar.
// Copy to ~/.config/hypr/shaders/crt.frag (with crt.json) and tag a window
// "shader" or "shaderbg" plus "shader:crt".
uniform float uScanline;   // 0..1 scanline darkness
uniform float uSplit;      // RGB split in px
uniform float uTintMix;    // 0..1 blend toward the palette colour

vec4 effect(vec2 fragCoord) {
    vec2 px = vUV * uSize;
    float dx = uSplit / uSize.x;
    vec3 c;
    c.r = source(vUV + vec2(dx, 0.0)).r;
    c.g = source(vUV).g;
    c.b = source(vUV - vec2(dx, 0.0)).b;
    float scan = 1.0 - uScanline * (0.5 + 0.5 * sin(px.y * 3.14159));
    float roll = 1.0 + 0.08 * smoothstep(0.0, 0.05, 0.05 - abs(fract(vUV.y - uTime * 0.15) - 0.5));
    vec3 tinted = mix(c, uFg.rgb * dot(c, vec3(0.2126, 0.7152, 0.0722)) * 1.4, uTintMix);
    return vec4(tinted * scan * roll, 1.0);
}
