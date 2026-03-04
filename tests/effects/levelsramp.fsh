#define input_white 0.75
#define gamma 2.2
#define anim_duration 0.67

float quadraticOut(float t) {
  return -t * (t - 2.0);
}

float quadraticInOut(float t) {
  float p = 2.0 * t * t;
  return t < 0.5 ? p : -p + (4.0 * t) - 1.0;
}

float cubicIn(float t) {
  return t * t * t;
}

float map(float value, float inMin, float inMax, float outMin, float outMax) {
  return outMin + (outMax - outMin) * (value - inMin) / (inMax - inMin);
}

float cmap(float value, float inMin, float inMax, float outMin, float outMax) {
  return clamp(outMin + (outMax - outMin) * (value - inMin) / (inMax - inMin), outMin, outMax);
}

void main() {
  vec2 uv = v_tex_coord.xy;
  vec4 col = texture2D(u_texture, uv);

  float introRatio = clamp((i_duration - anim_duration) / i_duration, 0.0, 1.0);
  float progress = clamp(mod(i_time, i_duration) / i_duration, 0.0, 1.0);
  float progressOut = cubicIn(cmap(progress, introRatio, 1.0, 0.0, 1.0));
    
  vec3 col_rgb_crushed = input_white * pow(col.rgb, 1.0 / gamma);
    
  col.rgb = mix(col.rgb, col_rgb_crushed, progressOut);
    
  gl_FragColor = col;
}
