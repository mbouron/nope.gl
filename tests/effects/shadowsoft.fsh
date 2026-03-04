#define pi 3.14159265

#define shadow_alpha 0.2
#define scale 0.95

void main()
{
    vec2 uv = v_tex_coord;
    
    vec2 shadow_offset = vec2(0.0025, 0.02);
    
    vec2 uv_scaled = vec2(1.0 / scale) * (uv - vec2(0.5)) + vec2(0.5);
    
    vec2 uv_front = uv_scaled - 0.5 * shadow_offset;
    float front_crop = step(0.0, uv_front.x) * step(uv_front.x, 1.0) * step(0.0, uv_front.y) * step(uv_front.y, 1.0);
    vec4 col = front_crop * texture2D(u_texture, uv_front);
    
    //float ratio = i_size.x / i_size.y;
    
    vec2 uv_shadow = uv_scaled + 0.5 * shadow_offset;
    
    
//    float shadow_alpha = back_crop * texture2D(u_texture, uv_back).a;
//    vec4 col_shadow = shadow_alpha * vec4(shadow_color, 1.0);
    
    // box blur
    vec2 blur_size = vec2(2.0);
    vec4 col_shadow = vec4(0.0);
    for (float y = -2.0; y <= 2.0; y += 1.0) {
        for (float x = -2.0; x <= 2.0; x += 1.0) {
            vec2 bb;
            bb.x = x * blur_size.x + 0.5 * sign(x);
            bb.y = y * blur_size.y + 0.5 * sign(y);
            bb /= i_size;
            float back_crop = step(0.0, uv_shadow.x + bb.x) * step(uv_shadow.x + bb.x, 1.0) * step(0.0, uv_shadow.y + bb.y) * step(uv_shadow.y + bb.y, 1.0);
            col_shadow += back_crop * texture2D(u_texture, uv_shadow + bb);
        }
    }
    col_shadow /= 25.0;
    
    vec4 shadow = vec4(vec3(0.0), shadow_alpha * col_shadow.a);
    
    gl_FragColor = mix(shadow, col, col.a);
}
