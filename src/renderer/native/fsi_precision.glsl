// Consumer raster epilogue. EDRAM addressing, color/depth encodings and blend
// ordering follow Xenia/ReXGlue's BSD-licensed FSI implementation. Attribution
// and license: texture_data.LICENSE. This consumes already-native shader output;
// it does not translate guest commands or shader instructions.
layout(set=0,binding=0,std430) coherent buffer NativeEdram {uint words[];} edram;
layout(push_constant) uniform NativeRasterState {
    uint pitch,color_base,depth_base,color_format;
    uint color_mask,blend_control,depth_control,depth_format;
    uint stencil_front,stencil_back,blend_enabled,reserved;
    vec4 blend_constant;
} raster;
uint edramWord(uvec2 p,uint base,bool depth,bool wide) {
    uint width=wide?40u:80u,multiplier=wide?2u:1u;
    uint tile=(p.y/16)*((raster.pitch+79)/80)*multiplier+p.x/width;
    uint column=p.x%width;
    if(depth)column=column<40?column+40:column-40;
    return ((base+tile)*1280+(p.y%16)*80+column*multiplier)%(2048*1280);
}
uint pack7e3(float v) {
    if(isnan(v))return 0;
    v=clamp(v,0.0,31.875);
    if(v<.25)return uint(roundEven(v*512.0));
    uint bits=floatBitsToUint(v)-0x3e000000u;
    return (bits+32767u+((bits>>16)&1u))>>16;
}
float unpack7e3(uint v) {uint e=v>>7,m=v&127u;return e==0?float(m)/512.0:float(m+128u)*exp2(float(int(e)-10));}
uint packFixed(float v) {precise float s=(isnan(v)?0.0:clamp(v,-32.0,32.0))*(32767.0/32.0);return uint(int(s+(s<0.0?-.5:.5)))&65535u;}
float unpackFixed(uint v) {return max(float(int(v<<16)>>16)/32767.0,-1.0)*32.0;}
uint packDepth(float v) {
    if(raster.depth_format==0)return uint(roundEven(clamp(isnan(v)?0.0:v,0.0,1.0)*16777215.0));
    if(!(v>0.0))return 0;
    uint b=floatBitsToUint(min(v,uintBitsToFloat(0x3ffffff8u)));
    uint p=b<0x38800000u?((0x800000u|(b&0x7fffffu))>>min(113u-(b>>23),24u)):b+0xc8000000u;
    return min((p+3+((p>>3)&1))>>3,0xffffffu);
}
vec4 unpackColor(uvec2 v) {
    uint f=raster.color_format;
    if(f==0)return vec4(v.x&255u,(v.x>>8)&255u,(v.x>>16)&255u,v.x>>24)/255.0;
    if(f==2)return vec4(v.x&1023u,(v.x>>10)&1023u,(v.x>>20)&1023u,v.x>>30)/vec4(1023,1023,1023,3);
    if(f==3)return vec4(unpack7e3(v.x&1023u),unpack7e3((v.x>>10)&1023u),unpack7e3((v.x>>20)&1023u),float(v.x>>30)/3.0);
    if(f==4||f==5)return vec4(unpackFixed(v.x&65535u),unpackFixed(v.x>>16),f==5?unpackFixed(v.y&65535u):0.0,f==5?unpackFixed(v.y>>16):1.0);
    if(f==6||f==7)return vec4(unpackHalf2x16(v.x),f==7?unpackHalf2x16(v.y):vec2(0,1));
    return vec4(uintBitsToFloat(v.x),f==15?uintBitsToFloat(v.y):0.0,0,1);
}
uvec2 packColor(vec4 c) {
    uint f=raster.color_format;
    if(f==0||f==2) {
        c=mix(c,vec4(0),isnan(c));precise vec4 scaled=clamp(c,0.0,1.0)*(f==0?vec4(255):vec4(1023,1023,1023,3));
        uvec4 v=uvec4(scaled+vec4(.5));
        return uvec2(f==0?v.x|(v.y<<8)|(v.z<<16)|(v.w<<24):v.x|(v.y<<10)|(v.z<<20)|(v.w<<30),0);
    }
    if(f==3) {precise float a=(isnan(c.w)?0.0:clamp(c.w,0.0,1.0))*3.0;return uvec2(pack7e3(c.x)|(pack7e3(c.y)<<10)|(pack7e3(c.z)<<20)|(uint(a+.5)<<30),0);}
    if(f==4||f==5)return uvec2(packFixed(c.x)|(packFixed(c.y)<<16),packFixed(c.z)|(packFixed(c.w)<<16));
    if(f==6||f==7) {c=clamp(mix(c,vec4(0),isnan(c)),-65504.0,65504.0);return uvec2(packHalf2x16(c.xy),packHalf2x16(c.zw));}
    return floatBitsToUint(c.xy);
}
uvec2 colorMask() {
    uint f=raster.color_format,m=raster.color_mask;uvec2 result=uvec2(0);
    for(uint c=0;c<4;++c)if((m&(1u<<c))!=0) {
        if(f==0)result.x|=255u<<(c*8);
        else if(f==2||f==3)result.x|=(c==3?3u:1023u)<<(c*10);
        else if(f>=4&&f<=7) {if(c<2||f==5||f==7)result[c/2]|=65535u<<((c&1u)*16);}
        else if(c==0||(c==1&&f==15))result[c]=0xffffffffu;
    }
    return result;
}
float fixedClamp(float v,uint component) {
    uint f=raster.color_format;
    bool fixed_point=f==0||f==2||f==4||f==5||(f==3&&component==3);
    if(!fixed_point)return v;
    float low=(f==4||f==5)?-32.0:0.0,high=(f==4||f==5)?32.0:1.0;
    return clamp(isnan(v)?0.0:v,low,high);
}
vec4 clampColor(vec4 v) {for(uint c=0;c<4;++c)v[c]=fixedClamp(v[c],c);return v;}
float factor(uint mode,uint c,vec4 source,vec4 dest,vec4 constant_color) {
    if(mode==0)return 0;
    if(mode==4)return source[c];if(mode==5)return 1.0-source[c];
    if(mode==6)return source.a;if(mode==7)return 1.0-source.a;
    if(mode==8)return dest[c];if(mode==9)return 1.0-dest[c];
    if(mode==10)return dest.a;if(mode==11)return 1.0-dest.a;
    if(mode==12)return constant_color[c];if(mode==13)return 1.0-constant_color[c];
    if(mode==14)return constant_color.a;if(mode==15)return 1.0-constant_color.a;
    if(mode==16)return c==3?1.0:min(source.a,1.0-dest.a);
    return 1.0;
}
float blendTerm(float value,uint mode,uint c,vec4 source,vec4 dest,vec4 constant_color) {
    if(mode==0)return 0.0; // Zero times NaN/Inf must contribute exact zero.
    precise float v=value*factor(mode,c,source,dest,constant_color);
    return fixedClamp(v,c);
}
vec4 blendColor(vec4 source,vec4 dest) {
    if(raster.blend_enabled==0)return source;
    source=clampColor(source);vec4 constant_color=clampColor(raster.blend_constant),result;
    for(uint c=0;c<4;++c) {
        uint state=c==3?raster.blend_control>>16:raster.blend_control;
        uint op=(state>>5)&7u;
        if(op==2||op==3) {
            result[c]=isnan(source[c])?dest[c]:isnan(dest[c])?source[c]:(op==2?min(source[c],dest[c]):max(source[c],dest[c]));
        } else {
            precise float a=blendTerm(source[c],state&31u,c,source,dest,constant_color);
            precise float b=blendTerm(dest[c],(state>>8)&31u,c,source,dest,constant_color);
            result[c]=op==1?a-b:op==4?b-a:a+b;
        }
    }
    return result;
}
bool compare(uint mode,uint a,uint b) {
    return mode==0?false:mode==1?a<b:mode==2?a==b:mode==3?a<=b:mode==4?a>b:mode==5?a!=b:mode==6?a>=b:true;
}
uint stencilOp(uint op,uint value,uint reference) {
    return op==0?value:op==1?0u:op==2?reference:op==3?min(value+1,255u):op==4?(value==0?0:value-1):op==5?(value^255u):op==6?((value+1)&255u):((value-1)&255u);
}
void nativeRasterOutput(vec4 color,float depth,bool front_face) {
    uvec2 p=uvec2(gl_FragCoord.xy);uint control=raster.depth_control;
    bool depth_enabled=(control&2u)!=0,stencil_enabled=(control&1u)!=0;
    bool pass=true;
    if(depth_enabled||stencil_enabled) {
        uint at=edramWord(p,raster.depth_base,true,false),previous=edram.words[at];
        uint incoming=packDepth(depth),reference_mask=front_face||(control&128u)==0?raster.stencil_front:raster.stencil_back;
        uint reference=reference_mask&255u,read_mask=(reference_mask>>8)&255u,write_mask=(reference_mask>>16)&255u;
        uint stencil_state=control>>(front_face||(control&128u)==0?8:20);
        uint stencil=previous&255u;
        bool stencil_pass=!stencil_enabled||compare(stencil_state&7u,reference&read_mask,stencil&read_mask);
        bool depth_pass=!depth_enabled||compare((control>>4)&7u,incoming,previous>>8);
        pass=stencil_pass&&depth_pass;
        if(stencil_enabled) {
            uint op=!stencil_pass?(stencil_state>>3)&7u:!depth_pass?(stencil_state>>9)&7u:(stencil_state>>6)&7u;
            uint result=stencilOp(op,stencil,reference);
            stencil=(stencil&~write_mask)|(result&write_mask);
        }
        uint packed_depth=pass&&depth_enabled&&(control&4u)!=0?incoming:previous>>8;
        edram.words[at]=(packed_depth<<8)|stencil;
    }
    if(pass&&raster.color_mask!=0) {
        bool wide=raster.color_format==5||raster.color_format==7||raster.color_format==15;
        uint at=edramWord(p,raster.color_base,false,wide);
        uvec2 previous=uvec2(edram.words[at],wide?edram.words[at+1]:0u);
        uvec2 packed=packColor(blendColor(color,unpackColor(previous))),mask=colorMask();
        packed=(previous&~mask)|(packed&mask); // Preserve noncanonical masked bytes.
        edram.words[at]=packed.x;if(wide)edram.words[at+1]=packed.y;
    }
}
