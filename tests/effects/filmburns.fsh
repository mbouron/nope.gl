#define pi 3.14159265

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

mat2 rotate2d(float _angle){
    return mat2(cos(_angle),-sin(_angle),
                sin(_angle),cos(_angle));
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
    
    float speed = 10.0;
    
    float noiseT = noise1d( 10.0 * floor(speed * s_time) );
    float noiseT2 = noise1d( 0.25 * floor(0.5 * speed * s_time) );
    
    // soft wiggle
    vec2 uvTexture = uv;
    uv.y += 1e-3 * step(0.5, noiseT);
    
    vec2 uvR = rotate2d( (pi / 8.0) * noiseT ) * uv;
    
    // float grain = -0.5 + random2d( uv + 0.01 * s_time);

    float n = 0.5 + 0.5 * noise( 0.5 * vec2(4.0, 1.0) * uvR + 10.0 * floor(speed * s_time));
    
    float t = 0.7 + 0.1 * step(0.5, noiseT2);
    float burn = smoothstep(t - 5e-2, t, n) * (1.0 + step(0.8, noiseT));
    
    float d = (n - t);
    
    // offset Y
    float t2 = 0.95;
    float d2 = (noiseT2 - t2);
    uv.y += 2e-1 * (1.0 + 3.0 * d2) * step(t2, noiseT2);
    if (uv.y > 1.0) {
        uv.y -= 1.0;
    }
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    // desaturate - lighten red hues
    float avg = 0.6 * col.r + 0.3 * col.g + 0.1 * col.b;
    col = mix(col, vec3(avg), 0.9);
    
    vec3 b = vec3(0.0);
    float a = 1.0;
    b.r = a * 0.3 * burn;
    b.g = a * 0.1 * (1.0 + 0.0 * d) * burn * noiseT * noiseT;
    
    // screen blend
    col = 1.0 - (1.0 - col) * (1.0 - b);
    
    // col *= 1.0 + 0.15 * grain;

    // Output to screen
    gl_FragColor = vec4(col, 1.0);
}
