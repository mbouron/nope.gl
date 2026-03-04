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

void main()
{
    float s_time = i_time;
    
    // Normalized pixel coordinates (from 0 to 1)
    vec2 uv = v_tex_coord;
    vec2 p = (2.0 * uv * u_size - u_size) / u_size.y;
    
    float ratio = u_size.x / u_size.y;
    
    float speed = 8.0;
    
    float noiseT = noise1d( floor(speed * s_time) );
    
    vec3 col = texture2D(u_texture, uv).rgb;

    // Properties
    const int octaves = 2;
    float lacunarity = 4.0;
    float gain = 0.7;
    //
    // Initial values
    float amplitude = 1.0;
    float frequency = 20.0;
    
    float offset1 = 10.0 * floor(speed * s_time);
    
    float amplitudeMax = 0.0;
    float noiseXY = 0.0;
    // Loop of octaves
    for (int i = 0; i < octaves; i++) {
        noiseXY += amplitude * noise(offset1 + frequency * uv);
        amplitudeMax += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    
    noiseXY /= amplitudeMax;
    noiseXY *= noiseXY * noiseXY;
    
    vec2 padding = vec2(4e-2, 6e-2);
    float radius = 8e-2;
    vec2 size = vec2(ratio, 1.0) - radius - padding;
    float d1 = sdRoundBox(p, size, radius);
    float d2 = sdRoundBox(p, size + 1e-3, radius);

    // outer border
    float v1 = 1.0 - smoothstep(0.0, 0.0 + 1e-2 + 1e-2 * noiseXY, d1 + 5e-2 * noiseXY);
    float v2 = 1.0 - smoothstep(0.0, 0.0 + 1e-2 + 1e-2 * noiseXY, d2 + 5e-2 * noiseXY);
    
    float fT = noise1d( 0.1 * speed * s_time );
    
    vec3 black = vec3(0.0);
    vec3 red = vec3(0.6, 0.2, 0.1);
    vec3 yellow = vec3(0.6, 0.5, 0.1);
    vec3 white = vec3(1.0);
    
    //intro
    float n = noise(1.5 * uv + 1e-2 * speed * s_time);
    float noiseXY2 = 5.0 * n;
    
    vec3 colBurn = mix(mix(mix(black, red, 0.5 + 0.5 * n), yellow, 0.5), white, 0.0);
    
    col = mix(vec3(0.0), col, 1.0 - 10.0 * smoothstep(0.1 * noiseT, 1.0, noiseXY));
    
    vec3 colMix = mix(2.0 * colBurn * col, 1.0 - 2.0 * (1.0 - colBurn) * (1.0 - col), step(vec3(0.5), col));
    col = mix(colMix, col, fT * noiseXY2);
    
    float tIntro = (1.0 - min(s_time, 1.0));
    float tBurn = 0.5 * smoothstep(0.8, 1.0, fract(0.2 * s_time));
    col += fract(6.0 * s_time) * red * (0.5 + n) * (tIntro + tBurn);

    vec3 glowMix = mix(red, yellow, max(3.0 * (v2 - 0.67), 0.0));
    vec3 outerCol = mix(black, glowMix, min(2.0 * v2, 1.0));
    col = mix(outerCol, col, v1);

    // Output to screen
    gl_FragColor = vec4(col, 1.0);
}
