
float luma(vec3 color) {
  return dot(color, vec3(0.299, 0.587, 0.114));
}

float random(float n){return fract(sin(n) * 43758.5453123);}

vec3 blendSoftLight(vec3 base, vec3 blend) {
    return mix(
        sqrt(base) * (2.0 * blend - 1.0) + 2.0 * base * (1.0 - blend),
        2.0 * base * blend + base * base * (1.0 - 2.0 * blend),
        step(base, vec3(0.5))
    );
}

vec3 applyGrain(vec2 uv, vec3 colorIn, float amount, float time) {
  vec3 noise = vec3(random(time + uv.x * time + uv.y*time) * .5 + random(time + uv.x * time * .65 + uv.y*time * .6 + .1245) * .5);
  vec3 color = blendSoftLight(colorIn, noise);
  color = mix(colorIn, color,  amount);
  return color;
}


void main()
{
    vec2 uv = v_tex_coord;
    vec4 col = texture2D(u_texture, uv);
    float grainAmount = 0.4;
    vec3 outputColor = applyGrain(v_tex_coord, col.rgb, grainAmount, i_time);

    gl_FragColor = vec4(outputColor, col.a);
}
