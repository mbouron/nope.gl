void main()
{
    float s_time = i_time;
    
    vec2 uv = v_tex_coord;
    
    vec2 coord = uv * u_size;

    float snow = 0.0;
    float random = fract(sin(dot(uv.xy,vec2(12.9898,78.233)))* 43758.5453);
    for(int k=0;k<6;k++){
        for(int i=0;i<12;i++){
            float cellSize = 2.0 + (float(i)*3.0);
            float downSpeed = 0.3+(sin(s_time*0.4+float(k+i*20))+1.0)*0.00008;
            vec2 uv = (coord / u_size.x) + vec2(0.01*sin((s_time+float(k*6185))*0.6+float(i))*(5.0/float(i)),downSpeed*(s_time+float(k*1352))*(1.0/float(i)));
            vec2 uvStep = (ceil((uv)*cellSize-vec2(0.5,0.5))/cellSize);
            float x = fract(sin(dot(uvStep.xy,vec2(12.9898+float(k)*12.0,78.233+float(k)*315.156)))* 43758.5453+float(k)*12.0)-0.5;
            float y = fract(sin(dot(uvStep.xy,vec2(62.2364+float(k)*23.0,94.674+float(k)*95.0)))* 62159.8432+float(k)*12.0)-0.5;

            float randomMagnitude1 = sin(s_time*2.5)*0.7/cellSize;
            float randomMagnitude2 = cos(s_time*2.5)*0.7/cellSize;

            float d = 5.0*distance((uvStep.xy + vec2(x*sin(y),y)*randomMagnitude1 + vec2(y,x)*randomMagnitude2),uv.xy);

            float omiVal = fract(sin(dot(uvStep.xy,vec2(32.4691,94.615)))* 31572.1684);
            if(omiVal<0.08?true:false){
                float newd = (x+1.0)*0.4*clamp(1.9-d*(15.0+(x*6.3))*(cellSize/1.4),0.0,1.0);
                /*snow += d<(0.08+(x*0.3))/(cellSize/1.4)?
                    newd
                    :newd;*/
                snow += newd;
            }
        }
    }
    
    vec4 col = texture2D(u_texture, uv);
    
    vec3 overlay = vec3(snow + random * 0.01);
    gl_FragColor = vec4(col.rgb + overlay, 1.0);
}
