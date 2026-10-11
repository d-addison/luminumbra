"""Synthetic contract fixtures only; no renderer execution or visual evidence."""
import copy
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest

from validate_foliage_evidence import Refusal, fnv, validate


def matrix():
    yaw, pitch = math.radians(35), math.radians(-18)
    front = (math.cos(yaw)*math.cos(pitch), math.sin(pitch), math.sin(yaw)*math.cos(pitch))
    right = (-math.sin(yaw), 0, math.cos(yaw))
    up = (-math.cos(yaw)*math.sin(pitch), math.cos(pitch), -math.sin(yaw)*math.sin(pitch))
    eye = (8, 12.4, -8)
    view = [[*right, -sum(a*b for a,b in zip(right,eye))],
            [*up, -sum(a*b for a,b in zip(up,eye))],
            [*(-x for x in front), sum(a*b for a,b in zip(front,eye))], [0,0,0,1]]
    f = 1/math.tan(math.radians(60)/2); near=.1; far=3200
    projection = [[f/(3840/1600),0,0,0], [0,f,0,0], [0,0,near/(far-near),far*near/(far-near)], [0,0,-1,0]]
    return [sum(projection[row][k]*view[k][col] for k in range(4)) for col in range(4) for row in range(4)]


def fixture(root):
    count = 100000
    records = []
    wind = []
    for i in range(64):
        sways = i != 63
        records.append(struct.pack('<5f4B2f2e',20,10,0,.035,.2,20,40,10,255 if sways else 0,0,0,0,0))
        wind.append(struct.pack('<5f4B2f2e',20,10,0,.035,.2,20,40,10,255 if sways else 0,6 if sways else 0,0,0,0))
    data = (b''.join(records)*(count//64+1))[:count*36]
    windy_data = (b''.join(wind)*(count//64+1))[:count*36]
    first_hash, windy_hash = fnv(data), fnv(windy_data)
    for name, payload in [('foliage-rebuild-first.bin',data),('foliage-rebuild-second.bin',data),('foliage-windy.bin',windy_data)]:
        (root/name).write_bytes(payload)
    phases = {}
    vertices = []
    gpu = []
    vp = matrix()
    for i, name in enumerate(('calm','windy')):
        capture, build, available, gen = (140,80,82,7) if i == 0 else (240,150,153,9)
        image = 'screenshots/'+name+'.ppm'
        (root/'screenshots').mkdir(exist_ok=True)
        (root/image).write_bytes(b'P6\n3840 1600\n255\n' + bytes(3840*1600*3))
        phases[name] = {'sampled':True,'phase':name,'instance_source':'gpu_readback',
            'instance_matches_drawn_build':True,'build_generation':gen,'instance_generation':gen,
            'readback_generation':gen,'build_frame':build,'instance_available_frame':available,
            'readback_consumed_frame':available,'capture_frame':capture,'width':3840,'height':1600,
            'fade_start_m':48,'fade_end_m':92,'density_scale':1.6,'time_of_day':.04,
            'shader_time_seconds':1,'wind_input_xz':[0,0] if i==0 else [6,0],
            'camera':{'position':[8,12.4,-8],'terrain_height_m':10,'yaw_degrees':35,
                      'pitch_degrees':-18,'vertical_fov_degrees':60},'screenshot':image,
            'instance_snapshot_hash':first_hash if i==0 else windy_hash}
        outputs=[]; height=struct.unpack('<f',struct.pack('<f',.2))[0]
        for blade in range(64):
            for j in range(12):
                tip=j%6 in (2,4,5); side=-1 if j%6 in (0,2,5) else 1
                angle=1.5707963 if j>=6 else 0
                world=[20+math.cos(angle)*side*.035 + (.05 if i and tip and blade!=63 else 0),
                       10+(height if tip else -.06),math.sin(angle)*side*.035]
                clip=[sum(vp[col*4+row]*(*world,1)[col] for col in range(4)) for row in range(4)]
                outputs.append(world+[int(tip)]+clip)
        vertices.append({'phase':i+1,'source_frame':capture,'available_frame':capture+1,'generation':gen,
            'instance_hash':first_hash if i==0 else windy_hash,'shader_time_seconds':1,'first_instance':0,
            'view_projection':vp,'vertices':outputs,'blade_heights':[height]*64,'sways':[True]*63+[False]})
        for j in range(32):
            gpu.append({'query_id':i*32+j+1,'source_frame':available+j,'phase':i+1,'generation':gen,
                        'instance_generation':gen,'instance_count':count,'shader_time_seconds':1,'milliseconds':.2})
    return {'schema':'luminumbra.foliage_instancing.v3','profile':'luminumbra.foliage_control.v2',
        'passed':True,'functional_control':{'passed':True},'gl_debug':{'errors':0},
        'qualification':{'visual_approved':False,'status':'complete','missing':[]},'phases':phases,
        'profile_settings':{'requested_density_multiplier':1},
        'coverage_density':{'instances_total':count,'minimum_instances':100000,'normalizer':873813,
            'instances_within_ring':count,'calibrated_count_ratio':count/873813,'biome_density':.3,'density_band':.6},
        'distance_fade':{'fade_start_m':48,'fade_end_m':92,'instances_beyond_fade':0},
        'render_pass':{'foliage_instances_drawn':count,'foliage_draws':1},
        'determinism':{'completed':True,'output_cleared':True,'byte_equal':True,'first_generation':6,
            'second_generation':7,'first_frame':70,'second_frame':80,'input_hash':99,'stride_bytes':36,
            'first_count':count,'second_count':count,'first_artifact':'foliage-rebuild-first.bin',
            'second_artifact':'foliage-rebuild-second.bin','first_hash':first_hash,'second_hash':first_hash},
        'rendered_motion':{'format':'world_xyz,height_t,clip_xyzw.float32','rasterizer_discard':False,
            'instances_per_phase':64,'vertices_per_instance':12,'windy_instance_artifact':'foliage-windy.bin',
            'phases':vertices},
        'gpu_timer':{'budget_ms':.6,'instrumented_vertex_frames_excluded':True,'samples':gpu,
            'gl_vendor':'synthetic fixture','gl_renderer':'synthetic fixture','gl_version':'synthetic fixture',
            'mean_ms':.2,'maximum_ms':.2}}


class EvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(); cls.root=Path(cls.temp.name)
        cls.base=fixture(cls.root)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def check(self, mutate=None):
        report=copy.deepcopy(self.base)
        if mutate: mutate(report)
        (self.root/'foliage-instancing-analysis.json').write_text(json.dumps(report))
        return validate(self.root)

    def test_complete_synthetic_fixture(self):
        result=self.check()
        self.assertTrue(result['passed']); self.assertFalse(result['visual_approved'])
        self.assertEqual(result['unique_gpu_frames'],64)

    def test_old_schema_refuses(self):
        with self.assertRaises(Refusal): self.check(lambda r:r.update(schema='luminumbra.foliage_instancing.v2'))

    def test_changed_raw_rebuild_refuses(self):
        path=self.root/'foliage-rebuild-second.bin'; before=path.read_bytes()
        try:
            path.write_bytes(b'X'+before[1:])
            with self.assertRaisesRegex(Refusal,'bytes differ'):self.check()
        finally:path.write_bytes(before)

    def test_raw_geometry_vertex_join(self):
        def mutate(r): r['rendered_motion']['phases'][0]['vertices'][0][0]+=1
        with self.assertRaisesRegex(Refusal,'actual instances'):self.check(mutate)

    def test_actual_image_frame_join(self):
        def mutate(r):r['rendered_motion']['phases'][1]['source_frame']+=1
        with self.assertRaisesRegex(Refusal,'frame join'):self.check(mutate)

    def test_shifted_camera_cannot_relabel_actual_vertex_draw(self):
        def mutate(r):
            for phase in r['phases'].values(): phase['camera']['position'][0] += 30
        with self.assertRaisesRegex(Refusal,'camera/actual draw projection'):self.check(mutate)

    def test_duplicate_gpu_source_refuses(self):
        def mutate(r):r['gpu_timer']['samples'][1]['source_frame']=r['gpu_timer']['samples'][0]['source_frame']
        with self.assertRaisesRegex(Refusal,'GPU source frame'):self.check(mutate)

    def test_instrumented_frame_refuses_budget_join(self):
        def mutate(r):r['gpu_timer']['samples'][31]['source_frame']=r['phases']['calm']['capture_frame']
        with self.assertRaisesRegex(Refusal,'GPU source frame'):self.check(mutate)

    def test_missing_timer_is_not_zero(self):
        def mutate(r):r['gpu_timer']['samples'][0]['milliseconds']=None
        with self.assertRaises(Refusal):self.check(mutate)

    def test_over_budget_sample_refuses(self):
        def mutate(r):
            r['gpu_timer']['samples'][0]['milliseconds']=.7
            r['gpu_timer']['maximum_ms']=.7;r['gpu_timer']['mean_ms']=(.7+63*.2)/64
        with self.assertRaisesRegex(Refusal,'0.6 ms'):self.check(mutate)

    def test_alias_rebuild_artifact_refuses(self):
        def mutate(r):r['determinism']['second_artifact']=r['determinism']['first_artifact']
        with self.assertRaisesRegex(Refusal,'distinct rebuild'):self.check(mutate)


if __name__=='__main__':unittest.main()
