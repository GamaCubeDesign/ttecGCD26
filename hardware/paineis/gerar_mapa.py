"""Desenha um mapa legível a partir dos manifests de posicionamento."""
from pathlib import Path
import json
from PIL import Image, ImageDraw, ImageFont

HERE=Path(__file__).resolve().parent
SCALE=1.65
PW,PH=415,528.6
MARGIN=32
GAP=55
HEADER=72
FOOTER=90
WIDTH=round(2*(PW*SCALE+2*MARGIN)+GAP)
HEIGHT=round(PH*SCALE+HEADER+FOOTER)

colors={
    'SOLAR_1':'#F8B26A','SOLAR_2':'#F09D68',
    'AOCS':'#67B8CD','EPS':'#80C799','OBC':'#B09CDD',
    'EPS_1_CIMA':'#E2CB68','EPS_2_BAIXO':'#EAB185',
    'topo':'#A5BAC6','TTEC':'#DC92A9',
}
labels={'EPS_1_CIMA':'EPS CIMA','EPS_2_BAIXO':'EPS BAIXO',
        'SOLAR_1':'SOLAR 1','SOLAR_2':'SOLAR 2'}
font_path=Path('C:/Windows/Fonts/arial.ttf')
font=ImageFont.truetype(str(font_path),17)
small=ImageFont.truetype(str(font_path),13)
title=ImageFont.truetype(str(font_path),24)
im=Image.new('RGB',(WIDTH,HEIGHT),'#f9fafb')
d=ImageDraw.Draw(im)

for panel in (1,2):
    data=json.loads((HERE/f'Painel_{panel}_mapa.json').read_text(encoding='utf-8'))
    x0=MARGIN+(panel-1)*(PW*SCALE+2*MARGIN+GAP)
    y0=HEADER
    d.text((x0,y0-43),f'PAINEL {panel}  |  415,0 x 528,6 mm',font=title,fill='#25313b')
    d.rectangle((x0,y0,x0+PW*SCALE,y0+PH*SCALE),fill='#d8dfe5',outline='#445461',width=2)
    for item in data['placements']:
        x=x0+item['x']*SCALE
        y=y0+item['y']*SCALE
        x2=x+item['width']*SCALE
        y2=y+item['height']*SCALE
        d.rounded_rectangle((x,y,x2,y2),radius=4.8,fill=colors[item['name']],outline='#334b55',width=2)
        name=labels.get(item['name'],item['name'])
        label=f'{name}  #{item["copy"]}'
        bb=d.textbbox((0,0),label,font=font)
        d.text((x+(x2-x-(bb[2]-bb[0]))/2,y+(y2-y-(bb[3]-bb[1]))/2-3),label,font=font,fill='#172b35')
        # Four tab locations, schematic only; drill coordinates are in KiCad.
        for yy in (y,y2):
            for frac in (1/3,2/3):
                cx=x+(x2-x)*frac
                d.line((cx-4,yy,cx+4,yy),fill='#ffffff',width=4)
    for x,y in data['tooling_holes_mm']:
        cx=x0+x*SCALE; cy=y0+y*SCALE
        d.ellipse((cx-3,cy-3,cx+3,cy+3),fill='#ffffff',outline='#25313b',width=1)
    for x,y in data['fiducials_mm']:
        cx=x0+x*SCALE; cy=y0+y*SCALE
        d.ellipse((cx-3,cy-3,cx+3,cy+3),fill='#ba7032',outline='#6c411e',width=1)
    pieces=len(data['placements'])
    d.text((x0,y0+PH*SCALE+13),f'{pieces} placas | {pieces*4} abas | {data["mousebite_holes"]} furos de picote | 3 furos CNC | 3 fiduciais',font=small,fill='#34434c')

d.text((MARGIN,HEIGHT-34),'Mapa esquemático. A geometria de fabricação está nos arquivos KiCad e Gerber.',font=small,fill='#586670')
im.save(HERE/'Mapa_Paineis.png',optimize=True)
