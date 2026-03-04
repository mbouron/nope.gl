#define c_iceColor vec3(0.11,0.39,0.61)
#define c_snowColor vec3(0.91,0.95,0.99)
#define c_seed 55.
#define c_IOR 0.001
#define c_lightX 0.2
#define c_lightY 0.2
#define c_roughness 0.4
#define c_specular 1.0
#define c_f0 2.0
#define c_introAnimationDelay 0.0
#define c_introAnimationDuration 1.5
#define c_introAnimationIOR 0.05
#define c_introAnimationEndFrost 0.4
vec2 hash(vec2 x)
{
  const vec2 k = vec2(0.3183099, 0.3678794);
  x = x * k + k.yx;
  return - 1.0 + 2.0 * fract(16.0 * k * fract(x.x * x.y * (x.x + x.y)));
}
float hash1(float n) { return fract(sin(n) * 43758.5453); }
vec2 hash2(vec2 p) { p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3))); return fract(sin(p) * 43758.5453); }
vec4 generateVoronoi(vec2 x, float w)
{
  vec2 n = floor(x);
  vec2 f = fract(x);

  vec4 m = vec4(8.0, 0.0, 0.0, 0.0);
  for (int j = -2; j <= 2; j++)
    for (int i = -2; i <= 2; i++)
    {
      vec2 g = vec2(float(i), float(j));
      vec2 o = hash2(n + g);

      float d = length(g - f + o);

      float h = smoothstep(0.0, 1.0, 0.5 + 0.5 * (m.x - d) / w);

      m.x = mix(m.x, d, h) - h * (1.0 - h) * w / (1.0 + 3.0 * w);
      m.yzw = mix(m.yzw, normalize(vec3(o * 2. - 1., 0.)), h) - h * (1.0 - h) * w / (1.0 + 3.0 * w);
    }

  return m;
}
float noise(vec2 p)
{
  vec2 i = floor(p);
  vec2 f = fract(p);

  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(dot(hash(i + vec2(0.0, 0.0)), f - vec2(0.0, 0.0)),
      dot(hash(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0)), u.x),
    mix(dot(hash(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0)),
      dot(hash(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0)), u.x), u.y);
}
vec3 noiseDerivative(vec2 p)
{
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
  vec2 ga = hash(i + vec2(0.0, 0.0));
  vec2 gb = hash(i + vec2(1.0, 0.0));
  vec2 gc = hash(i + vec2(0.0, 1.0));
  vec2 gd = hash(i + vec2(1.0, 1.0));

  float va = dot(ga, f - vec2(0.0, 0.0));
  float vb = dot(gb, f - vec2(1.0, 0.0));
  float vc = dot(gc, f - vec2(0.0, 1.0));
  float vd = dot(gd, f - vec2(1.0, 1.0));
  return vec3(va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd),
    ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
    du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va));
}
float fbm(vec2 x, float H)
{
  float G = exp2(-H);
  float f = 1.0;
  float a = 1.0;
  float t = 0.0;
  for (int i = 0; i < 10; i++)
  {
    t += a * noise(f * x);
    f *= 2.0;
    a *= G;
  }
  return t;
}
vec3 fbmDerivatives(vec2 position)
{
  float f = 2.;
  float s = .9;
  float a = 0.0;
  float b = .5;
  vec2 d = vec2(0.0);
  mat2 m = mat2(1.0, 0.0,
    0.0, 1.0);
  for (int i = 0; i < 9; i++)
  {
    vec3 n = noiseDerivative(position);
    a += b * n.x;
    d += b * m * n.yz;
    b *= s;
    position = f * position;
    m = f * m;
  }
  return vec3(a, normalize(d));
}
float ggx(vec3 N, vec3 V, vec3 L, float roughness, float F0) {
  float alpha = roughness * roughness;
  vec3 H = normalize(L - V);
  float dotLH = max(0.0, dot(L, H));
  float dotNH = max(0.0, dot(N, H));
  float dotNL = max(0.0, dot(N, L));
  float alphaSqr = alpha * alpha;
  float denom = dotNH * dotNH * (alphaSqr - 1.0) + 1.0;
  float D = alphaSqr / (3.141592653589793 * denom * denom);
  float F = F0 + (1.0 - F0) * pow(1.0 - dotLH, 5.0);
  float k = 0.5 * alpha;
  float k2 = k * k;
  return dotNL * D * F / (dotLH * dotLH * (1.0 - k2) + k2);
}
void main() {
  float aspectRatio = u_size.x / u_size.y;
  vec2 position = v_tex_coord;
  position.x *= aspectRatio;
  vec2 positionCentered = v_tex_coord * 2.0 - 1.0;
  positionCentered.x *= aspectRatio;
  vec3 fbmNoiseDerivative = fbmDerivatives(position);
  float s_time = i_play * i_time;
  float IOR = c_IOR;
  float introAnimationRatio = smoothstep(0.0, 1.0, (s_time - c_introAnimationDelay) / c_introAnimationDuration);
  introAnimationRatio = smoothstep(0.0, 1.0, introAnimationRatio);
  IOR += (1. - introAnimationRatio) * c_introAnimationIOR;
  vec4 voronoi = generateVoronoi(position * 2.0 + fbmNoiseDerivative.yz * 0.1 + c_seed, 0.02);
  vec3 color = texture2D(u_texture, v_tex_coord + voronoi.yz * IOR + fbmNoiseDerivative.yz * .1 * IOR).rgb;
  color = mix(color, color * 0.7 + c_iceColor * 0.3, IOR);
  vec3 light = normalize(vec3(c_lightX, c_lightY, 1.0));
  float frost = smoothstep(0., 0.5 + introAnimationRatio * (1. - c_introAnimationEndFrost), voronoi.x);
  float frostColor = smoothstep(0., 0.5, fbmNoiseDerivative.x);
  color = mix(color, c_snowColor, frost * frostColor);
  color += ggx(normalize(vec3((fbmNoiseDerivative.yz + voronoi.yz) * IOR, 1.0)), normalize(vec3(positionCentered, 1.)), light, c_roughness, c_f0) * 0.1 * c_specular;

  gl_FragColor = vec4(color, 1.0);
}
