#define pi 3.14159265
#define intensity 0.5

float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d(float x)
{
     float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(random1d(i), random1d(i + 1.0), smoothstep(0.,1.,f));
}

mat2 rotate2d(float _angle){
    return mat2(cos(_angle),-sin(_angle),
                sin(_angle),cos(_angle));
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
    
    vec2 uv = v_tex_coord;
    
    vec2 r = vec2(1.0, u_size.y / u_size.x);
    
    // scratches
    float s = 3.0;
    
    vec2 uvIndex = floor(s * uv * r);
    
    float offset1 = s * uvIndex.y + uvIndex.x;
    float offset2 = uvIndex.y + s * uvIndex.x;
    
    float rd1 = random1d(offset1);
    float rd2 = random1d(offset2);
    
    float tFactor = fract(.25 * s_time + s * rd1);
    float tStep = step(0.95, tFactor);

    vec2 uvS = rotate2d(pi * rd2) * uv * r;
    
    uvS = fract(s * uvS);
    
    float edge = step(0.1, uvS.y) * step(uvS.y, 0.9);
    
    float aa = 1.5e-3 * s;
    
    float f = 3.; float g = .2; float la = 3.; float a = 8e-2;
    const int oct = 3;
    
    float w = 0.0;
    for (int i = 0; i < oct; i++) {
        w += a * noise1d(f * uvS.y);
        a *= g;
        f *= la;
    }
    float l = uvS.x + w;
    float b = tStep * edge * smoothstep(0.5 - aa, 0.5, l) * (1.0 - smoothstep(0.5, 0.5 + aa, l));
    
    // dust
    const int octaves = 4;
    float lacunarity = 3.; float gain = .5; float amplitude = 1.; float frequency = 5.;
    
    float fps = 6.0;
    float offset3 = 20.0 * fract(floor(fps * s_time) / 20.0);
    
    float amplitudeMax = 0.0;
    float noise1 = 0.0;
    // Loop of octaves
    for (int i = 0; i < octaves; i++) {
        noise1 += amplitude * (0.5 + 0.5 * snoise(offset3 + frequency * uv));
        amplitudeMax += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    noise1 /= amplitudeMax;
    
    float threshold = 1e-1;
    float width = 3e-2;
    
    float dust = smoothstep(threshold, threshold + width, noise1);
    
    // final mix
    vec3 col = texture2D(u_texture, uv).rgb;
    col += intensity * 0.25 * b;
    col += intensity * 0.5 * (1.0 - dust);

    // Output to screen
    gl_FragColor = vec4(col, 1.0);
}

