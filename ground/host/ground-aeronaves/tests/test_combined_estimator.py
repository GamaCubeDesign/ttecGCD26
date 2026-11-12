import unittest
from copy import deepcopy
from ground.combined_estimator import CombinedEstimator


def context(phase='CLIMB', local='A', score=.9):
    candidate={'ident':local,'score':score,'evidence':{'initial_height_ft':2000,'height_above_airport_ft':2000,'distance_km':20}}
    return {'flight_state':phase,'data_quality':{'usable':True},'vertical_estimate':{'conflict':False},
            'tracking_episode':0,'aircraft_category':{'code':'A3'},'snapshot':{'trajectory':[{'event_ns':0}]},
            'airport_method':{'origin':{'candidate':candidate},'destination':{'candidate':candidate}},
            'origin_candidates':[candidate],'destination_candidates':[candidate]}


def pairs(routes=(('A','B',.9),), alignment=1):
    return {'status':'CANDIDATES','candidates':[{'origin':{'ident':a},'destination':{'ident':b},'score':s,
            'evidence':{'direction_alignment':alignment}} for a,b,s in routes]}


class CombinedTests(unittest.TestCase):
    def advance(self, model, ctx, p, times=(0,10,20)):
        for t in times:
            ctx['snapshot']['trajectory'][-1]['event_ns']=int(t*1e9)
            r=model.update(ctx,p,t,int(t*1e9))
        return r

    def test_climb_agreement_and_full_route_after_persistence(self):
        model=CombinedEstimator();ctx=context()
        initial=self.advance(model,ctx,pairs(),(0,))
        self.assertIsNone(initial['route'])
        r=self.advance(model,ctx,pairs(),(10,20))
        self.assertEqual(r['route'],{'origin':'A','destination':'B'})
        self.assertTrue(r['origin']['agreement'])
        self.assertEqual(r['origin']['weights'],{'local':.8,'pairs':.19999999999999996})
        self.assertEqual(r['destination']['weights']['pairs'],1)

    def test_descent_prioritizes_destination_level_prioritizes_geometry(self):
        ctx=context('DESCENT','C')
        r=self.advance(CombinedEstimator(),ctx,pairs())
        self.assertEqual(r['destination']['ident'],'C')
        self.assertEqual(r['destination']['weights']['local'],.8)
        self.assertIsNone(r['route'])  # A->C never supported as a complete route.
        ctx=context('LEVEL','C')
        ctx['airport_summary']={'takeoff':{'status':'ESTIMATED','airport':{'ident':'A','score':.9}}}
        r=self.advance(CombinedEstimator(),ctx,pairs())
        self.assertEqual(r['origin']['weights'],{'local':.3,'pairs':.7})
        self.assertEqual(r['route']['destination'],'B')

    def test_ambiguous_origin_partial_destination_and_no_multiplicity_bonus(self):
        p=pairs((('A','B',.9),('C','B',.89)))
        r=self.advance(CombinedEstimator(),context('LEVEL'),p)
        self.assertIsNone(r['origin']['ident'])
        self.assertEqual(r['destination']['ident'],'B')
        self.assertEqual(r['destination']['ranking'][0]['score'],.9)
        self.assertEqual(r['status'],'PARTIAL')

    def test_turns_conflicts_and_missing_method(self):
        r=self.advance(CombinedEstimator(),context('LEVEL'),pairs(alignment=.8))
        self.assertEqual(r['status'],'UNKNOWN')
        ctx=context();ctx['vertical_estimate']['conflict']=True
        self.assertEqual(self.advance(CombinedEstimator(),ctx,pairs())['status'],'UNKNOWN')
        r=self.advance(CombinedEstimator(),context(),{'status':'ERROR'})
        self.assertEqual(r['origin']['ident'],'A')
        self.assertIsNone(r['destination']['ident'])

    def test_duplicate_positions_episode_and_category_reset(self):
        m=CombinedEstimator();ctx=context()
        r=self.advance(m,ctx,pairs(),(0,0,0))
        self.assertIsNone(r['origin']['ident'])
        r=self.advance(m,ctx,pairs(),(10,20))
        self.assertIsNotNone(r['route'])
        ctx['tracking_episode']=1
        self.assertIsNone(self.advance(m,ctx,pairs(),(21,))['route'])
        self.advance(m,ctx,pairs(),(31,41))
        ctx['aircraft_category']['code']='A7'
        self.assertIsNone(self.advance(m,ctx,pairs(),(42,))['route'])

    def test_replacement_needs_margin_and_persistence(self):
        m=CombinedEstimator();ctx=context('LEVEL')
        self.advance(m,ctx,pairs(),(0,10,20))
        challenger=pairs((('C','B',.95),('A','B',.7)))
        self.assertEqual(self.advance(m,ctx,challenger,(30,))['origin']['ident'],'A')
        self.assertEqual(self.advance(m,ctx,challenger,(40,50))['origin']['ident'],'C')
        self.assertIsNone(self.advance(m,ctx,challenger,(120,))['route'])
