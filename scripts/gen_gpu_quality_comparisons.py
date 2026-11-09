#!/usr/bin/env python3
"""Create source-backed host CPU quality comparisons from real x264 decoding.
Outputs are synthetic host fixtures and CPU references, never PS4 captures.
No image libraries required: standard-library PPM/PNG and the existing FFmpeg.
SPDX-License-Identifier: GPL-3.0-only
"""
import argparse, hashlib, html, json, pathlib, struct, subprocess, zlib
WIDTH=1920;HEIGHT=1080
VARIANTS=('actual-suave-media','dither','dither-deband-suave','dither-deband-fuerte','dither-deband-suave-cas-media')
LABELS=('Actual: Suave + Media','Dither','Dither + Deband Suave','Dither + Deband Fuerte','Dither + Deband Suave + CAS Media')
GLYPHS={
 'X':('10001','10001','01010','00100','01010','10001','10001'),
 'C':('01111','10000','10000','10000','10000','10000','01111'),
 'L':('10000','10000','10000','10000','10000','10000','11111'),
 'O':('01110','10001','10001','10001','10001','10001','01110'),
 'U':('10001','10001','10001','10001','10001','10001','01110'),
 'D':('11110','10001','10001','10001','10001','10001','11110'),
 '4':('00010','00110','01010','10010','11111','00010','00010')}
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(args):
    result=subprocess.run([str(x) for x in args],capture_output=True,text=True)
    if result.returncode:raise RuntimeError(result.stderr)
    return result.stdout

def source_pixels(kind):
    pixels=bytearray(WIDTH*HEIGHT*3)
    for row in range(HEIGHT):
        line=bytearray(WIDTH*3)
        for x in range(WIDTH):
            if kind=='degradado-oscuro':
                value=12+(x*28)//(WIDTH-1)+(row*7)//(HEIGHT-1)
                rgb=(value,value+1,value+3)
            elif kind=='cielo-comprimido':
                noise=((x*0x9e3779b9^row*0x85ebca6b)>>11)%3-1
                value=(row*24)//(HEIGHT-1)
                rgb=(132+value+noise,174+value+noise,212+value+noise)
            else:
                value=28+(x*15)//(WIDTH-1)
                rgb=(value,value+2,value+5)
                if 350<=x<1570 and 250<=row<830:rgb=(25,31,39)
                if x in (350,351,1568,1569) and 250<=row<830:rgb=(212,217,224)
                if row in (250,251,828,829) and 350<=x<1570:rgb=(212,217,224)
            line[x*3:x*3+3]=bytes(rgb)
        pixels[row*WIDTH*3:(row+1)*WIDTH*3]=line
    if kind=='bordes-texto':
        scale=16
        for pos,char in enumerate('XCLOUD4'):
            glyph=GLYPHS[char]
            for gy in range(7):
                for gx in range(5):
                    if glyph[gy][gx]=='0':continue
                    left=550+pos*6*scale+gx*scale;top=420+gy*scale
                    for row in range(top,top+scale):
                        for x in range(left,left+scale):
                            start=(row*WIDTH+x)*3
                            pixels[start:start+3]=bytes((235,239,244))
        # Fine high-contrast edges expose halos and color ringing.
        for row in range(620,730):
            for x in range(500,1400):
                start=(row*WIDTH+x)*3
                pixels[start:start+3]=bytes((225,45,60) if (x//4)&1 else (30,210,230))
    return pixels

def write_png(path,width,height,pixels):
    def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data)&0xffffffff)
    filtered=b''.join(b'\0'+pixels[row*width*3:(row+1)*width*3] for row in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+
                     chunk(b'IDAT',zlib.compress(filtered,6))+chunk(b'IEND',b''))
def read_ppm(path):
    raw=path.read_bytes();header=b'P6\n1920 1080\n255\n'
    assert raw.startswith(header) and len(raw)==len(header)+WIDTH*HEIGHT*3
    return raw[len(header):]
def crop3(pixels,x,y,w=320,h=180):
    result=bytearray(w*h*27)
    for row in range(h):
        source=pixels[((y+row)*WIDTH+x)*3:((y+row)*WIDTH+x+w)*3]
        enlarged=b''.join(source[i:i+3]*3 for i in range(0,len(source),3))
        for repeat in range(3):result[((row*3+repeat)*w*3)*3:((row*3+repeat+1)*w*3)*3]=enlarged
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ffmpeg',default='ffmpeg');p.add_argument('--ffprobe',default='ffprobe')
    p.add_argument('--capture-binary',type=pathlib.Path,required=True)
    p.add_argument('--output',type=pathlib.Path,required=True)
    p.add_argument('--reuse',action='store_true',help='Reuse already encoded/decoded fixture bytes')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    fixtures=a.output/'fixtures';fixtures.mkdir(exist_ok=True)
    evidence=[];sections=[];fixture_sources=[];fixture_frames=[]
    definitions=(('degradado-oscuro','Degradado oscuro',(740,430)),
                 ('cielo-comprimido','Cielo / zona plana comprimida',(1060,310)),
                 ('bordes-texto','Bordes y texto',(540,400)))
    for kind,title,crop in definitions:
        source=fixtures/(kind+'-source.ppm');encoded=fixtures/(kind+'.mp4')
        decoded=fixtures/(kind+'-visible.nv12');padded=fixtures/(kind+'.nv12')
        if not a.reuse:
            source.write_bytes(b'P6\n1920 1080\n255\n'+source_pixels(kind))
            run([a.ffmpeg,'-hide_banner','-loglevel','error','-y','-loop','1','-framerate','30','-i',source,
                 '-t','1','-an','-vf','format=yuv420p','-c:v','libx264','-preset','medium','-tune','zerolatency',
                 '-b:v','180k','-maxrate','180k','-bufsize','180k','-pix_fmt','yuv420p',
                 '-color_primaries','bt709','-color_trc','bt709','-colorspace','bt709','-color_range','tv',encoded])
            run([a.ffmpeg,'-hide_banner','-loglevel','error','-y','-i',encoded,
                 '-vf','select=eq(n\\,15)','-frames:v','1','-pix_fmt','nv12','-f','rawvideo',decoded])
            raw=decoded.read_bytes();assert len(raw)==WIDTH*HEIGHT*3//2
            y=raw[:WIDTH*HEIGHT];uv=raw[WIDTH*HEIGHT:]
            padded.write_bytes(y+y[-WIDTH:]*8+uv+uv[-WIDTH:]*4)
        assert padded.stat().st_size==3133440
        probe=json.loads(run([a.ffprobe,'-v','error','-show_streams','-of','json',encoded]))
        info=probe['streams'][0];assert info['codec_name']=='h264' and info['color_space']=='bt709' and info['color_range']=='tv'
        fixture_sources.append({'path':encoded.name,'sha256':sha(encoded),'ffprobe':probe,'scene':kind})
        fixture_frames.append({'path':padded.name,'bytes':padded.stat().st_size,'sha256':sha(padded),
                               'pitch_y':1920,'storage_height':1088,'visible_height':1080,
                               'source_sha256':sha(encoded),'scene':kind,'decoded_frame_index':15})
        prefix=a.output/kind
        capture_log=run([a.capture_binary,padded,'1','0',prefix])
        (a.output/(kind+'-capture.log')).write_text(capture_log,encoding='utf-8')
        images=[];cards=[]
        for variant,label in zip(VARIANTS,LABELS):
            ppm=a.output/(kind+'-'+variant+'.ppm');pixels=read_ppm(ppm)
            full=ppm.with_suffix('.png');large=ppm.with_name(ppm.stem+'-crop3.png')
            write_png(full,WIDTH,HEIGHT,pixels);write_png(large,960,540,crop3(pixels,*crop))
            images.append({'variant':variant,'full':full.name,'sha256':sha(full),'crop3':large.name,'crop3_sha256':sha(large)})
            cards.append('<article><h3>'+html.escape(label)+'</h3><a href="'+full.name+'"><img src="'+full.name+'" width="480"></a><p>Recorte ×3, vecino más cercano</p><a href="'+large.name+'"><img src="'+large.name+'" width="480"></a></article>')
        evidence.append({'fixture':kind,'source_sha256':sha(source),'encoded_sha256':sha(encoded),
                         'decoded_sha256':sha(padded),'source_path':str(source.name),'decoded_path':str(padded.name),
                         'decoded_frame_index':15,'ffprobe':probe,'images':images,
                         'crop':{'x':crop[0],'y':crop[1],'width':320,'height':180,'scale':3}})
        sections.append('<h2>'+html.escape(title)+'</h2><div class="grid">'+''.join(cards)+'</div>')
        print('PASS comparison '+kind,flush=True)
    (fixtures/'manifest.json').write_text(json.dumps({'sources':fixture_sources,'frames':fixture_frames},indent=2)+'\n',encoding='utf-8')
    report={'scope':'Synthetic source -> real libx264 encode -> real H.264 decode -> CPU reference; no PS4 execution',
            'dimensions':[WIDTH,HEIGHT],'encoder_target_bitrate':180000,'fixtures':evidence}
    (a.output/'comparison-manifest.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    page='<!doctype html><meta charset="utf-8"><title>XCloud4 T25 — comparación CPU</title><style>body{font:16px system-ui;background:#12161b;color:#e6edf3;margin:24px}a{color:#93c5fd}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(480px,1fr));gap:16px}article{background:#1d2630;padding:14px}img{max-width:100%;height:auto;image-rendering:pixelated}</style><h1>XCloud4 TAREA 25 — comparación en host</h1><p>Imágenes sintéticas codificadas con libx264 a 180 kbps y decodificadas realmente. Referencia CPU 1920×1080. Son comparaciones en host; no son capturas de una PS4.</p><p>Actual usa el conversor CPU de producción sin modificar. Cada enlace abre la imagen a resolución original. Todas las variantes usan nitidez Media; CAS reemplaza la nitidez clásica.</p>'+''.join(sections)+'<p><a href="comparison-manifest.json">Manifiesto, metadatos y SHA-256</a></p>'
    (a.output/'index.html').write_text(page,encoding='utf-8')
if __name__=='__main__':main()

