#define scale 15.0
#define t1 0.996
#define aa 1e-2

#define stroke_width 6e-2

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
    
    float is_drawn = step(t1, tFactor1);
    float is_filled = step(0.5, tFactor2);

    vec2 uvS = fract(scale * uv);
    
    vec2 p = (2.0 * uvS * u_size - u_size) / u_size.y;
    
    float d_cross = sdCross(p, size, 0.0);
    
    // filled shapes
    float shapes = is_drawn * is_filled * (1.0 - smoothstep(0., aa, d_cross));
    
    // outlined crosses
    shapes += is_drawn * (1.0 - is_filled) * (1.0 - smoothstep(0.5 * stroke_width, 0.5 * stroke_width + aa, abs(d_cross)));
    
    vec3 col = texture2D(u_texture, uv).rgb;
    
    float time_factor = step(0.5, i_time);

    gl_FragColor = vec4(col + time_factor * shapes, 1.0);
}
