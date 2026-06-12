#version 100
precision mediump float;

uniform vec2 iResolution;
uniform float iTime;      /* seconds, wrapped to FOG_TIME_PERIOD by the app */
uniform vec3 uHighlight;

/* Drift follows a closed circle so the wrapped iTime never causes a visible
 * jump: one full revolution per FOG_TIME_PERIOD (2400 s).
 * DRIFT_ANGULAR = 2*pi / 2400 — keep in sync with FOG_TIME_PERIOD in wayland.c. */
#define DRIFT_ANGULAR 0.00261799388
#define DRIFT_RADIUS 8.5

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

/* 4 octaves max — keeps per-fragment cost low */
float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        v += amp * vnoise(p);
        p = p * 2.03 + vec2(13.7, 9.2);
        amp *= 0.5;
    }
    return v;
}

void main() {
    vec2 uv = gl_FragCoord.xy / iResolution.y;
    float ang = iTime * DRIFT_ANGULAR;
    vec2 drift = DRIFT_RADIUS * vec2(cos(ang), sin(ang));
    float f = fbm(uv * 3.0 + drift);
    f = f * f * (3.0 - 2.0 * f);
    vec3 base = vec3(0.04, 0.05, 0.08);
    vec3 col = mix(base, uHighlight, f * 0.55);
    gl_FragColor = vec4(col, 1.0);
}
