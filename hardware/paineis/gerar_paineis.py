"""Paineliza as nove PCBs com o pcbnew fornecido pelo KiCad 10.

Execute com o Python do KiCad 10, de dentro de hardware/paineis/ (no Windows:
C:/Users/jvara/AppData/Local/Programs/KiCad/10.0/bin/python.exe gerar_paineis.py).
Os caminhos de SRC foram ajustados em 22/10/2026 para as pastas de hardware/.
"""
from __future__ import annotations

from collections import Counter
from pathlib import Path
import json
import sys

import pcbnew as p

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(__file__).resolve().parent
SRC = {
    "AOCS": "aocs/CUBETSCHEK_AOCS.kicad_pcb",
    "EPS": "eps-potencia/PCB_EPS_3_EXTRA.kicad_pcb",
    "OBC": "obc/PCB_OBC.kicad_pcb",
    "EPS_1_CIMA": "bateria-cima/PCB_EPS_1_CIMA.kicad_pcb",
    "EPS_2_BAIXO": "bateria-baixo/PCB_EPS_2_BAIXO.kicad_pcb",
    "topo": "topo/topo.kicad_pcb",
    "TTEC": "ttec/pcb_ttec.kicad_pcb",
    "SOLAR_1": "painel-solar-1/PCB_SOLAR_PANEL.kicad_pcb",
    "SOLAR_2": "painel-solar-2/PCB_SOLAR_PANEL.kicad_pcb",
}
SOLAR = ("SOLAR_1", "SOLAR_2")
SQUARE = ("AOCS", "EPS", "OBC", "EPS_1_CIMA", "EPS_2_BAIXO", "topo", "TTEC")
BOTTOM_EXTRA = {1: ("EPS_1_CIMA", "EPS_2_BAIXO"), 2: ("EPS", "TTEC")}
EPS_ZONE_CLEARANCE = 0.20  # Effective clearance in the reviewed EPS .kicad_dru.
W, H = 415.0, 528.6
RAIL, GAP = 8.0, 3.0
TAB_W, BITE_D, BITE_PITCH = 5.0, 0.5, 0.8
KEEPALIVE=[]  # pcbnew SetOutline retains SWIG polygon storage.


def mm(x: float) -> int:
    return p.FromMM(x)


def vec(x: float, y: float) -> p.VECTOR2I:
    return p.VECTOR2I(mm(x), mm(y))


def duplicate(item):
    try:
        return item.Duplicate(False)
    except TypeError:
        return item.Duplicate()


def npth(board, x, y, diameter, ref):
    fp = p.FOOTPRINT(board)
    fp.SetReference(ref)
    fp.SetValue("NPTH")
    fp.Reference().SetVisible(False)
    fp.Value().SetVisible(False)
    fp.SetPosition(vec(x, y))
    pad = p.PAD(fp)
    pad.SetAttribute(p.PAD_ATTRIB_NPTH)
    pad.SetShape(p.PAD_SHAPE_CIRCLE)
    pad.SetSize(vec(diameter, diameter))
    pad.SetDrillSize(vec(diameter, diameter))
    # LSET.AllCuMask() is shared in the SWIG wrapper; mutating it changes
    # how *all* PTH pads serialize their mask layers. Build a fresh set.
    layers=p.LSET()
    for layer in (p.F_Cu,p.B_Cu,p.F_Mask,p.B_Mask):
        layers.AddLayer(layer)
    pad.SetLayerSet(layers)
    fp.Add(pad)
    board.Add(fp)


def fiducial(board, x, y, ref):
    fp = p.FOOTPRINT(board)
    fp.SetReference(ref)
    fp.SetValue("Fiducial_Donut_1_2_3")
    fp.Reference().SetVisible(False)
    fp.Value().SetVisible(False)
    fp.SetPosition(vec(x, y))
    pad = p.PAD(fp)
    pad.SetAttribute(p.PAD_ATTRIB_SMD)
    pad.SetShape(p.PAD_SHAPE_CIRCLE)
    pad.SetSize(vec(1.0, 1.0))
    layers=p.LSET()
    layers.AddLayer(p.F_Cu)
    layers.AddLayer(p.F_Mask)
    pad.SetLayerSet(layers)
    pad.SetLocalSolderMaskMargin(mm(0.5))
    fp.Add(pad)
    board.Add(fp)
    ring = p.PCB_SHAPE(board)
    ring.SetShape(p.SHAPE_T_CIRCLE)
    ring.SetStart(vec(x, y))
    ring.SetEnd(vec(x + 1.25, y))
    ring.SetWidth(mm(0.5))
    ring.SetLayer(p.F_Cu)
    board.Add(ring)


def edge_line(board, x1, y1, x2, y2, width=0.05, layer=p.Edge_Cuts):
    item = p.PCB_SHAPE(board)
    item.SetShape(p.SHAPE_T_SEGMENT)
    item.SetStart(vec(x1, y1))
    item.SetEnd(vec(x2, y2))
    item.SetWidth(mm(width))
    item.SetLayer(layer)
    board.Add(item)


def text_label(board, label, x, y):
    t = p.PCB_TEXT(board)
    t.SetText(label)
    t.SetPosition(vec(x, y))
    t.SetLayer(p.F_SilkS)
    t.SetTextSize(vec(1.0, 1.0))
    t.SetTextThickness(mm(0.15))
    board.Add(t)


def route_with_tabs(board, shape, ox, oy, sx1, sx2, old_y, new_y, tag, holes):
    # Substitute the straight horizontal side with three routed segments.
    x1, x2 = sorted((sx1 + ox, sx2 + ox))
    tab_centers = (x1 + (x2 - x1) / 3, x1 + 2 * (x2 - x1) / 3)
    intervals = [(x1, tab_centers[0] - TAB_W / 2),
                 (tab_centers[0] + TAB_W / 2, tab_centers[1] - TAB_W / 2),
                 (tab_centers[1] + TAB_W / 2, x2)]
    for a, b in intervals:
        copied = duplicate(shape)
        copied.SetStart(vec(a, new_y))
        copied.SetEnd(vec(b, new_y))
        copied.SetLayer(p.User_1)
        board.Add(copied)
    outside = -0.15 if old_y == "top" else 0.15
    for tab_i, center in enumerate(tab_centers, 1):
        for i in range(5):
            x = center + (i - 2) * BITE_PITCH
            y = new_y + outside
            npth(board, x, y, BITE_D, f"MB_{tag}_{old_y}_{tab_i}_{i+1}")
            holes.append((round(x, 3), round(y, 3), BITE_D))


def add_board(dest, source, name, index, x, y, holes, nets):
    bounds = source.GetBoardEdgesBoundingBox()
    x0 = p.ToMM(bounds.GetX()) + 0.075
    y0 = p.ToMM(bounds.GetY()) + 0.075
    # KiCad's bounding box includes half of the 0.15 mm Edge.Cuts line width.
    dx, dy = x - x0, y - y0
    offset = vec(dx, dy)
    tag = f"{name}_{index}"
    copper_limit=p.SHAPE_POLY_SET()
    if not source.GetBoardPolygonOutlines(copper_limit,False):
        raise ValueError(f"Contorno nao fechado: {name}")
    copper_limit.Inflate(-mm(0.30),p.CORNER_STRATEGY_ROUND_ALL_CORNERS,mm(0.02))
    KEEPALIVE.append(copper_limit)
    netmap = {}
    for oldnet in source.GetNetInfo().NetsByNetcode().values():
        oldname = oldnet.GetNetname()
        if not oldname:
            continue
        fullname = f"{tag}/{oldname}"
        newnet = p.NETINFO_ITEM(dest, fullname)
        dest.Add(newnet)
        netmap[oldname] = newnet
        nets.append(fullname)

    for i, fp in enumerate(source.GetFootprints(), 1):
        cp = p.Cast_to_FOOTPRINT(duplicate(fp))
        cp.Move(offset)
        cp.SetReference(f"{tag}_{i}_{fp.GetReference()}")
        # A unique internal reference is required, but the long generated
        # string must not be printed across a unit-board boundary.
        cp.Reference().SetVisible(False)
        cp.Value().SetVisible(False)
        for pad in cp.Pads():
            oldname = pad.GetNetname()
            if oldname:
                pad.SetNet(netmap[oldname])
        dest.Add(cp)
    for track in source.GetTracks():
        cp = duplicate(track)
        cp.Move(offset)
        oldname = track.GetNetname()
        if oldname:
            cp.SetNet(netmap[oldname])
        dest.Add(cp)
    for zone in source.Zones():
        cp = p.Cast_to_ZONE(duplicate(zone))
        # AOCS has a shaped ground zone. Preserve its outline. The other
        # source zones are simple oversized rectangles that rely on the
        # original Edge.Cuts to trim them; use the exact board perimeter.
        if name=='AOCS':
            zone_limit=p.SHAPE_POLY_SET(zone.Outline())
            zone_limit.Inflate(-mm(0.30),p.CORNER_STRATEGY_ROUND_ALL_CORNERS,mm(0.02))
            KEEPALIVE.append(zone_limit)
            cp.SetOutline(zone_limit)
        else:
            cp.SetOutline(copper_limit)
        cp.Move(offset)
        cp.SetZoneName(f"{tag}_{zone.GetZoneName()}")
        oldname = zone.GetNetname()
        if oldname:
            cp.SetNet(netmap[oldname])
        if name == 'EPS':
            # The source .kicad_dru applies 0.20 mm zone clearance even though
            # the zone itself stores 0.50 mm. The panel has no per-board DRU;
            # carry over the effective clearance so refilling stays connected.
            cp.SetLocalClearance(mm(EPS_ZONE_CLEARANCE))
        cp.UnFill()
        dest.Add(cp)

    bx1, by1, bx2, by2 = x, y, x + (198 if name in SOLAR else 83 if name != 'topo' else 82.8), y + (82.8 if name in SOLAR or name == 'topo' else 83)
    for drawing in source.GetDrawings():
        if isinstance(drawing, p.PCB_SHAPE) and drawing.GetLayer() == p.Edge_Cuts and drawing.GetShape() == p.SHAPE_T_SEGMENT:
            a, b = drawing.GetStart(), drawing.GetEnd()
            ax, ay, bx, by = (p.ToMM(a.x),p.ToMM(a.y),p.ToMM(b.x),p.ToMM(b.y))
            if abs(ay-by)<1e-4 and (abs(ay-y0)<0.1 or abs(ay-(y0+by2-by1))<0.1) and abs(ax-bx)>50:
                side = 'top' if abs(ay-y0)<0.1 else 'bottom'
                route_with_tabs(dest,drawing,dx,dy,ax,bx,side,ay+dy,tag,holes)
                continue
        cp = duplicate(drawing)
        cp.Move(offset)
        if isinstance(drawing,p.PCB_SHAPE) and drawing.GetLayer()==p.Edge_Cuts:
            shape=drawing.GetShape()
            if shape in (p.SHAPE_T_SEGMENT,p.SHAPE_T_ARC):
                pts=(drawing.GetStart(),drawing.GetEnd())
                outer=all(any(abs(p.ToMM(getattr(q,axis))-lim)<0.11 for axis,lim in
                              (('x',x0),('x',x0+bx2-bx1),('y',y0),('y',y0+by2-by1))) for q in pts)
                if outer:
                    cp.SetLayer(p.User_1)
        dest.Add(cp)
    return {'name':name,'copy':index,'x':round(bx1,3),'y':round(by1,3),
            'width':round(bx2-bx1,3),'height':round(by2-by1,3)}


def create(n):
    board = p.BOARD()
    board.SetCopperLayerCount(2)
    board.SetLayerName(p.User_1,"ROTEAMENTO_TABS")
    loaded = {k:p.LoadBoard(str(ROOT/v)) for k,v in SRC.items()}
    placements=[]; holes=[]; nets=[]
    # Two solar rows, then four rows of four square boards.
    for row in range(2):
        y=RAIL+row*(82.8+GAP)
        for col,name in enumerate(SOLAR):
            x=RAIL+col*(198+GAP)
            placements.append(add_board(board,loaded[name],name,n*2-1+row,x,y,holes,nets))
    square_seq = [name for name in SQUARE for _ in range(2)]
    rows=(square_seq[:4],square_seq[4:8],square_seq[8:12],
          (BOTTOM_EXTRA[n][0], *square_seq[12:], BOTTOM_EXTRA[n][1]))
    seen=Counter()
    for row, names in enumerate(rows):
        y=RAIL+2*(82.8+GAP)+row*(83+GAP)
        row_width=len(names)*83+(len(names)-1)*GAP
        x_start=(W-row_width)/2
        for col,name in enumerate(names):
            seen[name]+=1
            x=x_start+col*(83+GAP)
            first_copy=1 if n==1 else 4 if name in BOTTOM_EXTRA[1] else 3
            placements.append(add_board(board,loaded[name],name,first_copy+seen[name]-1,x,y,holes,nets))

    # Outer contour and assembly rails. Three tooling holes follow p. 36.
    for a,b,c,d in ((0,0,W,0),(W,0,W,H),(W,H,0,H),(0,H,0,0)):
        edge_line(board,a,b,c,d)
    for i,(x,y) in enumerate(((4,4),(W-4,4),(4,H-4)),1):
        npth(board,x,y,1.9,f"TOOL_{n}_{i}")
    for i,(x,y) in enumerate(((W/2,4),(W/2,H-4),(4,H/2)),1):
        fiducial(board,x,y,f"FID_{n}_{i}")
    text_label(board,f"PAINEL {n}  20 PLACAS",100,4)
    count=Counter(item['name'] for item in placements)
    if len(placements)!=20 or any(count[name]!=(3 if name in BOTTOM_EXTRA[n] else 2) for name in SRC):
        raise ValueError(f"Contagem incorreta de placas no painel {n}: {count}")
    if W>445 or H>590:
        raise ValueError('Painel fora da area util Circuibras 445 x 590 mm')
    for i,a in enumerate(placements):
        for b in placements[i+1:]:
            if (a['x']<b['x']+b['width'] and b['x']<a['x']+a['width'] and
                    a['y']<b['y']+b['height'] and b['y']<a['y']+a['height']):
                raise ValueError(f"Sobreposicao: {a} / {b}")
    mask_pads=sum(q.GetLayerSet().Contains(p.F_Mask) for f in board.GetFootprints() for q in f.Pads())
    if mask_pads<1000:
        raise ValueError(f"Mascara de solda perdida: apenas {mask_pads} pads")
    # Recompute all zones using the effective EPS clearance from its source.
    p.ZONE_FILLER(board).Fill(board.Zones())
    eps_zones = [z for z in board.Zones() if z.GetZoneName().startswith('EPS_') and z.GetZoneName().endswith('GND PLANE_1')]
    if len(eps_zones) != count['EPS']:
        raise ValueError(f"Quantidade de zonas EPS inesperada: {len(eps_zones)}")
    source_eps_zone = loaded['EPS'].Zones()[0]
    for z in eps_zones:
        for layer in (p.F_Cu, p.B_Cu):
            source_fill = source_eps_zone.GetFilledPolysList(layer)
            copy_fill = z.GetFilledPolysList(layer)
            if copy_fill.OutlineCount() != source_fill.OutlineCount():
                raise ValueError(f"Zona EPS fragmentada: {z.GetZoneName()} na camada {board.GetLayerName(layer)}")
            if abs(copy_fill.Area() - source_fill.Area()) > source_fill.Area() * 0.02:
                raise ValueError(f"Area de cobre EPS divergente: {z.GetZoneName()} na camada {board.GetLayerName(layer)}")
    output=OUT/f"Painel_{n}.kicad_pcb"
    p.SaveBoard(str(output),board)
    manifest={'file':output.name,'dimensions_mm':[W,H],'placements':placements,
              'net_count':len(nets),'mousebite_holes':len(holes),
              'tooling_holes_mm':[(4,4),(W-4,4),(4,H-4)],
              'fiducials_mm':[(W/2,4),(W/2,H-4),(4,H/2)],
              'routing_note':'Gerber User.1/ROTEAMENTO_TABS: fresagem pela linha, exceto lacunas de 5 mm nas bordas superior e inferior para abas perfuradas.'}
    (OUT/f"Painel_{n}_mapa.json").write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf-8')
    print(output, 'placements',len(placements),'nets',len(nets),'mousebite holes',len(holes))


if __name__=='__main__':
    for i in (1,2): create(i)
