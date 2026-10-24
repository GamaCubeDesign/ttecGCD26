"""Lista textos de serigrafia visíveis que cruzam a borda das PCBs de origem."""
import pcbnew as p
from gerar_paineis import ROOT, SRC

for name,path in SRC.items():
    board=p.LoadBoard(str(ROOT/path))
    outline=board.GetBoardEdgesBoundingBox()
    margin=p.FromMM(0.5)
    x1=outline.GetX()-margin; y1=outline.GetY()-margin
    x2=outline.GetRight()+margin; y2=outline.GetBottom()+margin
    found=[]
    items=list(board.GetDrawings())
    for fp in board.GetFootprints():
        items.extend((fp.Reference(),fp.Value()))
        items.extend(fp.GraphicalItems())
    for item in items:
        if item.GetLayer() not in (p.F_SilkS,p.B_SilkS):
            continue
        if not hasattr(item,'GetText') or not item.IsVisible():
            continue
        bb=item.GetBoundingBox()
        if bb.GetX()<x1 or bb.GetY()<y1 or bb.GetRight()>x2 or bb.GetBottom()>y2:
            found.append((item.GetText(),round(p.ToMM(bb.GetX()),2),round(p.ToMM(bb.GetY()),2)))
    print(name,found[:15], 'total',len(found))
