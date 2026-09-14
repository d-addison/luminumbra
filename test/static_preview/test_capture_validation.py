"""Synthetic stdlib-only refusal tests; no renderer or native process is launched."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[2] / 'tools/static_preview/validate_capture.py'
spec = importlib.util.spec_from_file_location('static_capture_validator', MODULE)
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
IDENTITY = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]


def sha(data): return hashlib.sha256(data).hexdigest()
def write(path, value):
    data=(json.dumps(value,sort_keys=True)+'\n').encode();path.write_bytes(data);return sha(data)


class CaptureValidationTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='static-capture-fixture-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.output=self.root/'output';self.output.mkdir()
        self.request_path=self.root/'request.json';self.expect_path=self.root/'expect.json'
        self.generation='1'*32;self.project=self.root/'project'
        generation=self.project/'.luminumbra-author/generations'/self.generation;generation.mkdir(parents=True)
        mirror=IDENTITY.copy();mirror[0]=-1
        descriptor={'schema':'luminumbra.asset.prefab.v1','nodes':[
            {'id':'root','parent':None,'local_matrix':IDENTITY},
            {'id':'child','parent':'root','local_matrix':mirror,'mesh':'mesh'}],
            'meshes':{'mesh':[{'file':'mesh.lmesh','material':'paint'}]},
            'materials':{'paint':{'alpha_mode':'OPAQUE'}}}
        mesh=b'synthetic pinned binary; no renderer evidence';(generation/'mesh.lmesh').write_bytes(mesh)
        write(generation/'prefab.json',descriptor)
        manifest={'schema':'luminumbra.authoring.generation.v1','job_id':self.generation,'outputs':{
            name:{'bytes':(generation/name).stat().st_size,'sha256':sha((generation/name).read_bytes()),'triangles':1}
            for name in ('mesh.lmesh','prefab.json')}}
        self.manifest=write(generation/'manifest.json',manifest)
        projection=[0.0]*16;projection[0]=projection[5]=1;projection[10]=.1/99.9;projection[11]=-1;projection[14]=10/99.9
        camera={'view':IDENTITY,'projection':projection,'width':2,'height':2,'revision':1,'near_plane':.1,'far_plane':100}
        update=IDENTITY.copy();update[12]=2
        camera2=dict(camera,revision=2)
        self.request={'schema':'luminumbra.static_preview.request.v1','prefabs':[{
            'id':'fixture','project_root':'C:/native/project','generation_id':self.generation,'manifest_sha256':self.manifest}],
            'instances':[{'id':'one','prefab':'fixture','placement':IDENTITY}],
            'frames':[{'camera':camera},{'camera':camera2,'updates':[{'instance_id':'one','node_id':'root','local':update}]}]}
        self.expect={'source_commit':'2'*40,'source_dirty':False,'source_input_sha256':'3'*64,
            'executable_sha256':'4'*64,'module_sha256':'5'*64,'pid':1234,'exit_code':0,'timed_out':False,'cancelled':False,
            'project_roots':{'fixture':str(self.project)},'required_renderer':'synthetic',
            'resources':{name:'6'*64 for name in ['static_asset.vert','static_asset.frag','lighting_pass.vert','lighting_pass.frag','config_constants.gen.glsl']}}
        self.capture={k:self.expect[k] for k in ['source_commit','source_dirty','source_input_sha256','executable_sha256','module_sha256','pid','resources']}
        self.capture.update(schema='luminumbra.static_preview.capture.v1',status='complete',shutdown_complete=True,visual_approved=False,
            watchdog_seconds=60,profile='static-perspective-opaque-mask-v1',generations={'fixture':{'generation_id':self.generation,'manifest_sha256':self.manifest}},frames=[])
        self.frames=[]
        for index in range(2):
            directory=self.output/('frame-'+str(index+1));directory.mkdir()
            depth=struct.pack('<4f',0,.5,1,0);coverage=bytes([0,1,1,0]);color=bytes([1,2,3,0,10,20,30,255,40,50,60,255,1,2,3,0])
            model=mirror.copy();model[12]=2 if index else 0
            frame={'sequence':index+1,'scene_revision':index+1,'camera':self.request['frames'][index]['camera'],
                'actual_view':IDENTITY,'actual_projection':[struct.unpack('<f',struct.pack('<f',x))[0] for x in projection],
                'origin':'bottom-left','deferred_position_format':'RGB32F','draws':1,'indices':3,'mesh_uploads':0 if index else 1,
                'texture_uploads':0,'updated_instances':1,'synchronous_render_readback_ms':.2,
                'gpu':{'vendor':'synthetic fixture','renderer':'synthetic fixture','version':'synthetic fixture'},
                'instances':[{'instance_id':'one','node_id':'child','primitive_index':0,'material_id':'paint','model':model,
                    'normal':[-1,0,0,0,1,0,0,0,1],'reverse_front_face':True,'generation_id':self.generation,'manifest_sha256':self.manifest}]}
            for name,filename,payload in [('color','color.rgba8',color),('depth','depth.f32le',depth),('coverage','coverage.u8',coverage)]:
                (directory/filename).write_bytes(payload);frame[name]={'file':filename,'bytes':len(payload),'sha256':sha(payload)}
            frame['color'].update(encoding='production-inspection-power-2.2',alpha='straight-coverage')
            frame['depth'].update(range=[0,1],near=1,clear=0,projection='RH-ZO-reversed-finite')
            frame['coverage']['covered_pixels']=2;self.frames.append(frame)
        self.pin()

    def pin(self):
        request_digest=write(self.request_path,self.request);self.expect['request_sha256']=request_digest;self.capture['request_sha256']=request_digest
        self.capture['frames']=[]
        for index,frame in enumerate(self.frames):
            directory='frame-'+str(index+1)
            self.capture['frames'].append({'directory':directory,'receipt_sha256':write(self.output/directory/'frame.json',frame)})
        self.expect['capture_sha256']=write(self.output/'capture.json',self.capture);write(self.expect_path,self.expect)

    def check(self): return validator.validate(self.output,self.request_path,self.expect_path)
    def refuse(self,pattern):
        with self.assertRaisesRegex(validator.Refusal,pattern): self.check()

    def test_complete_pinned_two_frame_fixture(self):
        result=self.check();self.assertTrue(result['passed']);self.assertFalse(result['visual_approved']);self.assertEqual(len(result['frames']),2)

    def test_external_capture_pin_required(self):
        self.capture['pid']=9;write(self.output/'capture.json',self.capture);self.refuse('external capture')

    def test_external_module_and_process_refusal(self):
        self.capture['module_sha256']='7'*64;self.pin();self.refuse('module_sha256')
        self.capture['module_sha256']=self.expect['module_sha256'];self.expect['exit_code']=124;self.pin();self.refuse('external process')

    def test_exact_gpu_camera_float_join(self):
        self.frames[0]['actual_projection'][0]=1.00001;self.pin();self.refuse('GPU matrix')

    def test_wrong_forward_depth_label_and_projection(self):
        self.frames[0]['depth']['clear']=1;self.pin();self.refuse('depth convention')

    def test_duplicate_frame_sequence(self):
        self.frames[1]['sequence']=1;self.pin();self.refuse('sequence')

    def test_scene_revision_and_mirror_normal_join(self):
        self.frames[1]['scene_revision']=1;self.pin();self.refuse('scene revision')
        self.frames[1]['scene_revision']=2;self.frames[1]['instances'][0]['normal'][0]=1;self.pin();self.refuse('normal mismatch')

    def test_relabelled_geometry_transform_refuses(self):
        self.frames[1]['instances'][0]['model'][12]=3;self.pin();self.refuse('model mismatch')

    def test_generation_member_tamper_refuses(self):
        path=self.project/'.luminumbra-author/generations'/self.generation/'mesh.lmesh';path.write_bytes(b'changed');self.refuse('generation member')

    def test_resource_external_pin_refuses(self):
        self.capture['resources']=dict(self.capture['resources']);self.capture['resources']['lighting_pass.frag']='8'*64;self.pin();self.refuse('resource hash')

    def test_nan_depth_even_with_updated_hash_refuses(self):
        data=struct.pack('<4f',0,float('nan'),1,0);path=self.output/'frame-1/depth.f32le';path.write_bytes(data)
        self.frames[0]['depth']['sha256']=sha(data);self.pin();self.refuse('raw reversed depth')

    def test_coverage_alpha_mismatch_even_with_updated_hash(self):
        path=self.output/'frame-1/color.rgba8';data=bytearray(path.read_bytes());data[7]=0;path.write_bytes(data)
        self.frames[0]['color']['sha256']=sha(data);self.pin();self.refuse('alpha/coverage')

    def test_transform_edit_cannot_reupload_meshes(self):
        self.frames[1]['mesh_uploads']=1;self.pin();self.refuse('rebuilt geometry')

    def test_hang_or_partial_output_refuses(self):
        (self.output/'hang.json').write_text('{}');self.refuse('failure/hang')

    def test_symlink_and_traversal_refuse(self):
        path=self.output/'frame-1/color.rgba8';original=path.read_bytes();outside=self.root/'outside';outside.write_bytes(original)
        path.unlink();path.symlink_to(outside);self.refuse('symlink/reparse')
        path.unlink();path.write_bytes(original);self.frames[0]['color']['file']='../outside';self.pin();self.refuse('plane name')

    def test_duplicate_json_key_refuses(self):
        raw=(self.output/'capture.json').read_bytes().replace(b'"pid": 1234',b'"pid": 1234, "pid": 1234')
        (self.output/'capture.json').write_bytes(raw);self.expect['capture_sha256']=sha(raw);write(self.expect_path,self.expect);self.refuse('duplicate JSON')

    def test_gpu_identity_changes_between_frames(self):
        self.frames[1]['gpu']['vendor']='another fixture';self.pin();self.refuse('GPU changed')

    def test_no_coverage_requires_explicit_external_expectation(self):
        self.expect['minimum_covered_pixels']=[3,1];self.pin();self.refuse('insufficient expected coverage')


if __name__ == '__main__': unittest.main()
