#define LAYERS 15
#define DEPTH 1.
#define WIDTH .8
#define SPEED 1.

void main()
{
    const mat3 p = mat3(13.323122,23.5112,21.71123,21.1212,28.7312,11.9312,21.8112,14.7212,61.3934);
    
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    vec2 uvR = vec2(1.0, u_size.y / u_size.x) * uv;

    vec3 acc = vec3(0.0);
    float dof = 5.*sin(s_time*.1);
    for (int i=0;i<LAYERS;i++) {
        float fi = float(i);
        vec2 q = uvR*(1.+fi*DEPTH);
        q += vec2(q.y*(WIDTH*mod(fi*7.238917,1.)-WIDTH*.5),SPEED*s_time/(1.+fi*DEPTH*.03));
        vec3 n = vec3(floor(q),31.189+fi);
        vec3 m = floor(n)*.00001 + fract(n);
        vec3 mp = (31415.9+m)/fract(p*m);
        vec3 r = fract(mp);
        vec2 s = abs(mod(q,1.)-.5+.9*r.xy-.45);
        s += .01*abs(2.*fract(10.*q.yx)-1.);
        float d = .6*max(s.x-s.y,s.x+s.y)+max(s.x,s.y)-.01;
        float edge = .005+.05*min(.5*abs(fi-5.-dof),1.);
        acc += vec3(smoothstep(edge,-edge,d)*(r.x/(1.+.02*fi*DEPTH)));
    }
    
    vec4 col = texture2D(u_texture, uv);
    gl_FragColor = vec4(col.rgb + vec3(acc), 1.0);
}
