#define pi 3.14159265
#define loop_duration 40.0

float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d(float x)
{
     float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(random1d(i), random1d(i + 1.0), smoothstep(0.,1.,f));
}

float random2d(vec2 st) {
    return fract(sin(dot(st.xy,
                         vec2(12.9898,78.233)))*
        43758.5453123);
}

vec2 random2(vec2 st){
    st = vec2( dot(st,vec2(127.1,311.7)),
              dot(st,vec2(269.5,183.3)) );
    return -1.0 + 2.0 * fract(sin(st)*43758.5453123);
}

float noise(vec2 st) {
    vec2 i = floor(st);
    vec2 f = fract(st);

    vec2 u = f*f*(3.0-2.0*f);

    return mix( mix( dot( random2(i + vec2(0.0,0.0) ), f - vec2(0.0,0.0) ),
                     dot( random2(i + vec2(1.0,0.0) ), f - vec2(1.0,0.0) ), u.x),
                mix( dot( random2(i + vec2(0.0,1.0) ), f - vec2(0.0,1.0) ),
                     dot( random2(i + vec2(1.0,1.0) ), f - vec2(1.0,1.0) ), u.x), u.y);
}

float sdRoundBox(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b;
    return min(max(q.x,q.y),0.0) + length(max(q,0.0)) - r;
}

vec2 screenDistort(vec2 uv)
{
    uv -= vec2(0.5);
    return uv * (1.0 + 1.5 * uv.x * uv.x * uv.y * uv.y) + vec2(0.5);
}

void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    uv = screenDistort(uv);
    vec2 p = (2.0 * uv * u_size - u_size) / u_size.y;
    
    float ratio = u_size.x / u_size.y;
    
    float speed = 8.0;
    
    float noiseT = noise1d( floor(speed * s_time) );
    
    vec3 col = texture2D(u_texture, uv).rgb;

    // Properties
    const int octaves = 4;
    float lacunarity = 5.0;
    float gain1 = 0.4; float gain2 = 0.5;
    //
    // Initial values
    float amplitude1 = 1.0; float amplitude2 = 1.0;
    float frequency = 2.0;
    
    float offset1 = 0.0; float offset2 = lacunarity * frequency;
    
    float amplitudeMax1 = 0.0; float amplitudeMax2 = 0.0;
    float noiseXY = 0.0; float noiseXY2 = 0.0;
    // Loop of octaves
    for (int i = 0; i < octaves; i++) {
        if (i == octaves - 1) {
            offset1 = 10.0 * floor(speed * s_time);
            offset2 = lacunarity * frequency + 10.0 * floor(speed * s_time);
        }
        noiseXY += amplitude1 * noise(offset1 + frequency * uv);
        noiseXY2 += amplitude2 * noise(offset2 + frequency * uv);
        amplitudeMax1 += amplitude1;
        amplitudeMax2 += amplitude2;
        frequency *= lacunarity;
        amplitude1 *= gain1;
        amplitude2 *= gain2;
    }
    
    noiseXY /= amplitudeMax1;
    noiseXY2 /= amplitudeMax2;
    noiseXY *= noiseXY * noiseXY;
    
    float paddingH = 2e-2;
    float paddingV = 32e-2;
    vec2 padding = vec2(paddingH, paddingV);
    float radius = 1e-1;
    vec2 size = vec2(ratio, 1.0) - radius - padding;
    
    vec2 offsetP1 = 5e-3 * vec2(noise1d(10. * s_time), noise1d(10. + 10. * s_time));
    float d1 = sdRoundBox(p + offsetP1, size, radius);
    float d2 = sdRoundBox(p + offsetP1, size, radius);
    
    float radius2 = 3e-1;
    vec2 size2 = vec2(ratio, 1.0) - radius2 - padding;
    float dDust = sdRoundBox(p + offsetP1, size2 - 2e-2, radius2);
    
    float radiusNotch = 3e-2;
    
    float heightNotch = min(0.5 * paddingV / 3.0, 0.15);
    vec2 sizeNotch = vec2(2. * heightNotch, heightNotch) - radiusNotch;
    vec2 offsetP2 = vec2(0., 1. - .5 * paddingV) + 5e-3 * vec2(noise1d(20. + 10. * s_time), noise1d(30. + 10. * s_time));
    vec2 mid = vec2(ratio, 0.);
    //vec2 uvNotch = fract(uv + vec2(0.5, 0.0));
    //vec2 pNotch = (2.0 * uvNotch * u_size - u_size) / u_size.y;
    float dNotchInner = sdRoundBox(p + offsetP2, sizeNotch, radiusNotch);
    float dNotchOuter = sdRoundBox(p + offsetP2, sizeNotch + 1e-3, radiusNotch);
    
    //float noiseA = mix(0.0, 1.2, dNotchOuter);
    
    float vNotchInner = smoothstep(0., 1e-2, dNotchInner);
    float vNotchOuter = smoothstep(0., 1e-2, dNotchOuter + 1.5 * noiseXY);
    
    float vNotch = (1. - vNotchOuter) * vNotchInner;

    // outer border
    float v1 = max(min(1.0 - smoothstep(0., 1e-2 + 1e-2 * noiseXY, d1 + 1.2 * noiseXY), vNotchInner), vNotch);
    float v2 = max(min(1.0 - smoothstep(-1e-2, 1e-2 + 1e-2 * noiseXY, d2 + 1.2 * noiseXY), vNotchInner), vNotch);
    
    float vDust = smoothstep(0., 5e-2, dDust + .5 * noiseXY2);

    vec3 black = vec3(0.0);
    vec3 gold = vec3(1., .95, .3);
    vec3 orange = vec3(1., .7, 0.);
    vec3 yellowish = vec3(1., .8, 0.);
    vec3 white = vec3(1.0);
    
    // dust
    float threshold = 1.5e-1;
    float width = 5e-2;
    float offsetDust = 2000. * fract(floor(4. * i_time) / 20.);
    float dust = smoothstep(
        threshold,
        threshold + width,
        0.5 + 0.5 * noise(offsetDust + 60.0 * uv));
    col -= 0.5 * (1.0 - dust);
    
    // grain
    // float grain = -0.5 + random2d( uv + 0.01 * s_time);
    // col *= 1.0 + 0.1 * grain;
    
    // border dust
    col = mix(col, 1. - (1. - yellowish) / col, 0.1 * vDust);

    vec3 glowMix = mix(orange, gold, step(vNotchOuter, 0.5));
    vec3 outerCol = mix(black, glowMix, min(2. * v2, 1.0));
    col = mix(outerCol, col, v1);

    // Output to screen
    gl_FragColor = vec4(col, 1.0);
}


