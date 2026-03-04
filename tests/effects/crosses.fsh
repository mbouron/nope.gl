#define scale 15.0
#define t1 0.998
#define t2 0.5
#define t3 0.97
#define aa 1e-3

float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float sdCross(vec2 p, vec2 b, float r)
{
    p = abs(p); p = (p.y>p.x) ? p.yx : p.xy;
    
    vec2  q = p - b;
    float k = max(q.y,q.x);
    vec2  w = (k>0.0) ? q : vec2(b.y-p.x,-k);
    
    return sign(k)*length(max(w,0.0)) + r;
}

void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    
    vec2 r = vec2(u_size.x / u_size.y, 1.0);
    
    vec2 size = 0.5 * vec2(1.0, 0.25);
    
    vec2 uvIndex = floor(scale * uv);
    float offset1 = scale * uvIndex.y + uvIndex.x;
    float offset2 = uvIndex.y + scale * uvIndex.x;
  
    float rd1 = random1d(offset1);
    float rd2 = random1d(offset2);
   
    float tFactor1 = fract(1e-2 * s_time + rd1);
    float tFactor2 = fract(1e-2 * s_time + rd2);
    
    float isDrawn = step(t1, tFactor1);
    float isFilled = step(t2, tFactor2);

    vec2 uvS = fract(scale * uv);
    
    vec2 p = (2.0 * uvS * u_size - u_size) / u_size.y;
    
    float c = sdCross(p, size, 0.);
    
    // filled crosses
    float crosses = isDrawn * isFilled * (1.0 - smoothstep(0., aa, c));
    
    // outlined crosses
    const float w = 6e-2;
    crosses += isDrawn * (1.0 - isFilled) * (1.0 - smoothstep(0.5 * w - aa, 0.5 * w, c)) * smoothstep(-0.5 * w - aa, -0.5 * w, c);
    
    // lined crosses
    vec2 uvS2 = fract(scale * uv);
    uvS2 += vec2(0.25, 0.0) * sin(2.0 * s_time);
    vec2 p2 = (2.0 * uvS2 * u_size - u_size) / u_size.y;
    vec2 size2 = 0.25 * vec2(1.0, 0.1);
    float c2 = sdCross(p2, size2, 0.);

    vec2 uvIndex2 = floor(0.25 * scale * uv);
    float offset3 = scale * uvIndex2.y + uvIndex2.x;

    float rd3 = random1d(offset3);

    float tFactor3 = fract(6e-2 * s_time + rd3);
    float isDrawn2 = step(t3, tFactor3);
    crosses += isDrawn2 * 0.5 * (1.0 - smoothstep(0., aa, c2));
    
    vec3 col = texture2D(u_texture, uv).rgb;

    gl_FragColor = vec4(col + crosses, 1.0);
}
