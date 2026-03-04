#define pi 3.14159265

#define lacunarity 2.0
#define gain 2.0
#define frequency 80.0
#define octaves 4

#define grain_amount 0.5

float rand(float x) {
    return fract(sin(x)*100000.0);
}

float random (vec2 st) {
    return fract(sin(dot(st.xy,
                         vec2(12.9898,78.233)))*
        43758.5453123);
}

float noise1d(float x)
{
     float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(rand(i), rand(i + 1.0), smoothstep(0.,1.,f));
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
    vec2 uv = v_tex_coord;
    
    vec2 r = vec2(u_size.x / u_size.y, 1.0);
    
    float speed = 8.0;
    float s_time = i_time;
    
    float random_t = rand( floor(20. * s_time) );
    
    float amplitude = 1.0;
    float f = frequency;

    float offset = random_t * frequency;
    
    float grain = 0.0;

    float amplitudeSum = 0.0;
    for (int i = 0; i < octaves; i++) {
        grain += amplitude * (0.5 * snoise(offset + r * f * uv) + 0.5);
        
        amplitudeSum += amplitude;
        f *= lacunarity;
        amplitude *= gain;
    }
    
    grain /= amplitudeSum;
    
    float paddingH = -0.5;
    float paddingV = 4e-1;
    float radius = 0.0;
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    vec3 grain_overlay = step(.5, col) * (1. - (1. - 2. * (col - 0.5) ) * (1. - grain) );
    grain_overlay += step(col, vec3(.5)) * ( (2. * col) * grain );
    
    col = mix(col, grain_overlay, grain_amount);
    
    gl_FragColor = vec4(col, 1.0);
}


