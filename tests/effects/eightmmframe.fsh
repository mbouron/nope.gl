#define pi 3.14159265

#define octaves 4

#define frequency 5.0
#define lacunarity 3.0
#define gain 0.8

#define grain_lacunarity 2.0
#define grain_gain 2.0
#define grain_frequency 80.0

#define grain_amount 0.4

#define black vec3(0.0)

#define paddingH 1.5e-2
#define paddingV 1.5e-2
#define radius 3e-2

#define dust_threshold 2e-5
#define dust_width 2e-2
#define dust_color vec3(1., 1., .5)

#define border_dust_radius 6e-2

float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d(float x)
{
     float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(random1d(i), random1d(i + 1.0), smoothstep(0.,1.,f));
}

float sdRoundBox(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b;
    return min(max(q.x,q.y),0.0) + length(max(q,0.0)) - r;
}

vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289(((x*34.0)+1.0)*x); }

float snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187,  // (3.0-sqrt(3.0))/6.0
                        0.366025403784439,  // 0.5*(sqrt(3.0)-1.0)
                        -0.577350269189626,  // -1.0 + 2.0 * C.x
                        0.024390243902439); // 1.0 / 41.0
    vec2 i  = floor(v + dot(v, C.yy) );
    vec2 x0 = v -   i + dot(i, C.xx);
    vec2 i1;
    i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289(i); // Avoid truncation effects in permutation
    vec3 p = permute( permute( i.y + vec3(0.0, i1.y, 1.0 ))
        + i.x + vec3(0.0, i1.x, 1.0 ));

    vec3 m = max(0.5 - vec3(dot(x0,x0), dot(x12.xy,x12.xy), dot(x12.zw,x12.zw)), 0.0);
    m = m*m ;
    m = m*m ;
    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * ( a0*a0 + h*h );
    vec3 g;
    g.x  = a0.x  * x0.x  + h.x  * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

void main()
{
    float s_time = i_time;
    
    // Normalized pixel coordinates (from 0 to 1)
    vec2 uv = v_tex_coord;
    vec2 p = (2.0 * uv * u_size - u_size) / u_size.y;
    
    vec2 r = vec2(u_size.x / u_size.y, 1.0);
    
    float ratio = u_size.x / u_size.y;
    
    vec2 offset = vec2(0.0, 2e-3 * (noise1d( 10. * s_time ) - 0.5));
    
    vec3 col = texture2D(u_texture, uv + offset).rgb;
    
    // RGB shift
    col.r = texture2D(u_texture, uv + offset + vec2(3e-3, 0.)).r;

    // Initial values
    float amplitude1 = 1.0; float grain_amplitude = 1.0;
    float f = frequency;
    
    float grain_f = grain_frequency;
    
    float random_t = random1d( floor(20. * s_time) );
    float grain_offset = random_t * grain_frequency;
    
    float amplitudeMax1 = 0.0; float amplitudeMax2 = 0.0; float amplitudeMaxGrain = 0.0;
    float noiseXY = 0.0; float grain = 0.0;
    // Loop of octaves
    for (int i = 0; i < octaves; i++) {
        noiseXY += amplitude1 * (0.5 * snoise(f * uv) + 0.5);
        grain += grain_amplitude * (0.5 * snoise(grain_offset + grain_f * r * uv) + 0.5);
        
        amplitudeMax1 += amplitude1;
        amplitudeMaxGrain += grain_amplitude;

        f *= lacunarity;
        grain_f *= grain_lacunarity;

        amplitude1 *= gain;
        grain_amplitude *= grain_gain;
    }
    
    noiseXY /= amplitudeMax1;
    grain /= amplitudeMaxGrain;
    noiseXY = 1e-2 * pow(noiseXY, 5.0);
    
    // grain
    vec3 grain_overlay = step(.5, col) * (1. - (1. - 2. * (col - 0.5) ) * (1. - grain) );
    grain_overlay += step(col, vec3(.5)) * ( (2. * col) * grain );
    
    col = mix(col, grain_overlay, grain_amount);

    vec2 padding = vec2(paddingH, paddingV);

    vec2 size = vec2(ratio, 1.0) - radius - padding;
    
    float d1 = sdRoundBox(p, size, radius);
    
    vec2 size2 = vec2(ratio, 1.0) - border_dust_radius - padding;
    float dDust = sdRoundBox(p, size2 - 1.5e-2, border_dust_radius);

    // outer border
    float v1 = 1.0 - smoothstep(0., 5e-3, d1 + 0.5 * noiseXY);
    float vDust = smoothstep(0., 5e-2 + 10e-2 * noiseXY, dDust + 8.0 * noiseXY);
        
    vec2 s2 = size - 2e-2 - r * 6e-2;
    float d2 = sdRoundBox(p, s2, 2e-2);
    
    float t2 = 5e-3;
    float v2 = smoothstep(-t2, 0., d2) * (1. - smoothstep(0., t2, d2));

    // dust
    float dust_offset = 2000. * fract(floor(3. * i_time) / 50.);
    float dust = smoothstep(
        dust_threshold,
        dust_threshold + dust_width,
        0.5 + 0.5 * snoise(20.0 * r * (uv + dust_offset)));
    
    col += dust_color * 0.5 * (1.0 - dust);
    
    // border dust
    col = mix(col, black, 0.5 * vDust);

    col = mix(black, col, v1);
    
    vec2 corner_cut = step(1. - 0.3 / r, 2. * abs(uv - 0.5));
    float frame_opacity = 0.03 + 0.06 * corner_cut.x * corner_cut.y;
    col += -frame_opacity * v2;

    // Output to screen
    gl_FragColor = vec4(col, 1.0);
}
