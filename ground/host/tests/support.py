"""Helpers shared by the estimator's and the host's tests."""
import sqlite3


def pair_db(path,coords=((0,-2),(0,2)),priorities=None):
    with sqlite3.connect(path) as db:
        db.executescript('''CREATE TABLE airports(id INTEGER PRIMARY KEY,ident TEXT,name TEXT,type TEXT,latitude_deg REAL,longitude_deg REAL);
          CREATE TABLE metadata(key TEXT,value TEXT); CREATE TABLE regional_metadata(key TEXT,value TEXT);
          CREATE TABLE regional_pairs(airport_a_id INTEGER,airport_b_id INTEGER,analysis_priority INTEGER);''')
        db.executemany('INSERT INTO airports VALUES (?,?,?,?,?,?)',[(i+1,'P'+str(i+1),'Airport','large_airport',lat,lon) for i,(lat,lon) in enumerate(coords)])
        pairs=[(a+1,b+1,(priorities or {}).get((a+1,b+1),0)) for a in range(len(coords)) for b in range(a+1,len(coords))]
        db.executemany('INSERT INTO regional_pairs VALUES (?,?,?)',pairs)
        db.executemany('INSERT INTO metadata VALUES (?,?)',[('generation_status','complete'),('pair_count',str(len(pairs)*2))])
        db.executemany('INSERT INTO regional_metadata VALUES (?,?)',[('status','complete'),('bonus_enabled','false'),('unordered_pair_count',str(len(pairs)))])
