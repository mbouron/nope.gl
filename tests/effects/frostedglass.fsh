#define pi 3.14159265
#define COLORIZE 1.0
#define COLOR_RGB vec3(0.7, 1.0, 1.0)
#define loop_duration 3.0

float rand(vec2 uv) {
 
    float a = dot(uv, vec2(92., 80.));
    float b = dot(uv, vec2(41., 62.));
    
    float x = sin(a) + cos(b) * 51.;
    return fract(x);
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
    
    // Properties
    const int octaves = 4;
    float lacunarity = 5.0;
    float gain = 0.7;

    // Initial values
    float amplitude = 1.0;
    float frequency = 1.5;
    
    vec3 offset = vec3(0.0, 5e-2, 3e-2);
    
    vec3 n = vec3(0.0);
    float amplitudeSum = 0.0;
    
    vec2 warp = vec2(1.0, 4.0) * vec2(u_size.x / u_size.y, 1.0);
    // Loop of octaves
    for (int i = 0; i < octaves; i++) {
        n.r += amplitude * (0.5 + 0.5 * snoise(offset.r + frequency * warp * uv));
        n.g += amplitude * (0.5 + 0.5 * snoise(offset.g + frequency * warp * uv));
        n.b += amplitude * (0.5 + 0.5 * snoise(offset.b + frequency * warp * uv));
        amplitudeSum += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    n /= amplitudeSum;

    vec3 d = smoothstep(0.4, 0.8, n);
    vec2 rnd = vec2(rand(uv+d.r*.05), rand(uv+d.b*.05));
    
    //vignette
    vec2 lensRadius = vec2(0.65*1.5, 0.05);
    float dist = distance(uv.xy, vec2(0.5,0.5));
    float vigfin = 1.-smoothstep(lensRadius.x, lensRadius.y, dist);
    vigfin *= vigfin;
    
    // frost animation
    float frostyness = 0.25 + 1.25 * s_time / loop_duration;
   
    rnd *= 0.025 * vigfin+d.rg * frostyness * vigfin;
    uv += rnd;
    
    vec4 col = texture2D(u_texture, uv);
    
    vec3 colOutput = mix(col.rgb, COLOR_RGB, COLORIZE * rnd.r);
    gl_FragColor = vec4(colOutput, 1.0);
}
