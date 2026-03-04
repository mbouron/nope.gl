#define scale 1.0
#define pi 3.14159265
#define N_LAYERS 3
#define loop_duration 10.0
#define spaceFactor 7.0

float rand(float x) {
    return fract(sin(x)*100000.0);
}

float heart(vec2 p) {
    // Center it more, vertically:
    p.y += .6;
    // This offset reduces artifacts on the center vertical axis.
    const float offset = .3;
    // (x^2+(1.2*y-sqrt(abs(x)))^2−1)
    float k = 1.2 * p.y - sqrt(abs(p.x) + offset);
    return p.x * p.x + k * k - 1.;
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

void main()
{
    vec2 uv = v_tex_coord;
    vec2 p = (2.0 * uv * u_size - u_size) / u_size.y;
    
    //uv *= scale;
    
    vec3 redPink = vec3(206,47,92) / vec3(255.0);
    vec3 redPunchy = vec3(226,15,62) / vec3(255.0);
    
    float s_time = i_time;
    
    vec2 center = vec2(0.5);
    
    vec3 colDots = vec3(0.0);
    
    vec2 pIndex = floor(scale * p);
    float index1 = pIndex.x + scale * pIndex.y;
    float index2 = scale * pIndex.x + pIndex.y;
            
    for (int i = 0; i < N_LAYERS; i++) {
        float padding = float(i) * scale * spaceFactor;
        float rd1 = rand(index1 + padding);
        float rd2 = rand(index2 + padding);

        float timeOffset = index1 + padding;

        float fadeT = noise1d(1.0 * (timeOffset + s_time));
        fadeT *= fadeT;
        float blinkT = noise1d(0.5 * floor(8.0 * (timeOffset + s_time)));
        float alpha = fadeT * (6e-1 + 1e-1 * rd1 - 0.25 * blinkT);
        float softness = 1e-1 * (1. + 4. * step(rd2, 0.67));

        float angle1 = 2. * pi * rd1;
        float angle2 = 2. * pi * rd2;
        vec2 offset = .7 * spaceFactor * rd2 * vec2(cos(angle1), sin(angle1));
        vec2 dir = rd2 * vec2(cos(angle2), sin(angle2));
        
        float speedB = 3e-2 * (5. + 4. * rd1);

        vec2 pRand = spaceFactor * 2.0 * (fract(scale * p) - 0.5) + offset + speedB * s_time * dir;

        float dist = heart(pRand);

        vec3 c = mix(redPunchy, redPink, 0.5 + 0.5 * rd1);

        float borderFill = 1. - smoothstep(-.5 * softness, .5 * softness, dist);
        float borderStroke = (1. - smoothstep(0., 1e-1, dist)) * smoothstep(-1e-1, 0., dist);

        colDots += c * alpha * mix(0.5, 1.0, step(rd2, 0.33)) * borderFill;
        colDots += c * alpha * step(0.33, rd2) * borderStroke;
    }
    vec4 col = texture2D(u_texture, uv);
    
    gl_FragColor = vec4(col.rgb + colDots, 1.0);
}
