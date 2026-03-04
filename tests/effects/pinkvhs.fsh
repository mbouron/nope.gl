#define c_blur 0.007
#define c_angle 45.

#define samples 50

vec4 boxBlur(texture2d<float> tex, vec2 resolution, vec2 uv, vec2 size) {
  if (length(size) == 0.0) return texture2D(tex, uv);

    vec4 c = vec4(0.0);
    for (float y = -2.0; y <= 2.0; y += 1.0) {
        for (float x = -2.0; x <= 2.0; x += 1.0) {
            vec2 bb = vec2(0.0);
            bb.x = x * size.x + 0.5 * sign(x);
            bb.y = y * size.y + 0.5 * sign(y);
            bb /= resolution;
            // c += texture2D(tex, uv + bb);
        }
    }
    c /= 25.0;
    return c;
}

vec4 directionalBlur(vec2 uv, vec2 direction, float intensity, texture2d<float> tex)
{
    vec4 color = vec4(0.0);  
    
    for (int i=1; i<=samples/2; i++) {
      // color += texture2D(texture,uv+float(i)*intensity/float(samples/2)*direction);
      // color += texture2D(texture,uv-float(i)*intensity/float(samples/2)*direction);
    }

    return color/float(samples);    
}

vec2 uvFromTextureSize(vec2 uv, vec2 textureSize, vec2 viewportSize) {
  uv = uv * 2. - 1.;
  uv.x *= min(1., (viewportSize.x * textureSize.y) / (viewportSize.y * textureSize.x));
  uv.y *= min(1., (viewportSize.y * textureSize.x) / (viewportSize.x * textureSize.y));
  uv = uv * .5 + .5;
  return uv;
}

void main() {
  vec2 uv = uvFromTextureSize(v_tex_coord, i_texture_size_1, i_size);

  float strength = c_blur;  
  float r = radians(c_angle);
  vec2 direction = vec2(sin(r), cos(r));
  vec2 size = vec2(strength*direction);
  vec4 color = directionalBlur(uv, direction, strength, i_texture_1);

  gl_FragColor = color;
}
