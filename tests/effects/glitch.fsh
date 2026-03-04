float random1d(float x) {
    return fract(sin(x)*100000.0);
}

float noise1d(float x)
{
     float i = floor(x);  // integer
    float f = fract(x);  // fraction
    return mix(random1d(i), random1d(i + 1.0), smoothstep(0.,1.,f));
}

float random2d (vec2 st) {
    return fract(sin(dot(st.xy,
                         vec2(12.9898,78.233)))*
        43758.5453123);
}

void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    vec2 block = floor(v_tex_coord * vec2(16));
    float speed = 10.0;
    
    float noiseT = noise1d( 10.0 * floor(speed * s_time) );
    //float noiseX = rand( floor(8.0 * uv.x) + 10.0 * floor(0.5 * speed * iTime));
    float noiseY = random1d( floor(16.0 * uv.y) + 8.0 * floor(1.0 * speed * s_time));
    
    vec2 c = vec2(0.5);
    vec2 uvC = uv - c;
    float d = dot(uvC, uvC);
    
    vec2 uv_r = uv, uv_g = uv, uv_b = uv;
    
    // float grain = -0.5 + random2d( uv + 0.01 * s_time);
    
    float t1 = 0.98;
    float d1 = (noiseY - 0.5) * 0.5 * step(t1, noiseY);
    uv_r += d1 * 0.15;
    uv_g += d1 * 0.1;
    uv_b += d1 * 0.08;
    
    float t2 = 0.8;
    float d2 = (noiseT - 0.5) * step(t2, noiseT);
    float mult = 1e-2 * d * d * (1.0 + 4.0 * d2);
    uv_r += 5.0 * mult;
    uv_b -= 2.0 * mult;
    
    vec4 colR = texture2D(u_texture, uv_r);
    vec4 colG = texture2D(u_texture, uv_b);
    vec4 colB = texture2D(u_texture, uv_g);
    
    vec3 col;
    
    // bw glitch
    //col.r = (colR.r + colR.g + colR.b) / 3.0;
    //col.g = (colG.r + colG.g + colG.b) / 3.0;
    //col.b = (colB.r + colB.g + colB.b) / 3.0;
    
    col.r = colR.r;
    col.g = colG.g;
    col.b = colB.b;
    
    // col += 0.1 * grain;
    

    gl_FragColor = vec4(col, 1.0);
}
