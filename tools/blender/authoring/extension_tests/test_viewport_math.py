"""Numerical fixtures for viewport coordinates; no rendered qualification claim."""
import importlib.util
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("viewport_math", Path(__file__).parents[1] / "extension/viewport_math.py")
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)


def projection(near=.1, far=100., aspect=4/3):
    s = 1 / math.tan(math.pi / 8)
    return (s/aspect,0,0,0, 0,s,0,0, 0,0,-(far+near)/(far-near),-1,
            0,0,-2*near*far/(far-near),0)


def orthographic(near=.1, far=100.):
    # Off-center volume x [-2,6], y [-2,4], matching a 4:3 image.
    return (.25,0,0,0, 0,1/3,0,0, 0,0,-2/(far-near),0,
            -.5,-1/3,-(far+near)/(far-near),1)


class ViewportMath(unittest.TestCase):
    def close(self, first, second, places=10):
        for a,b in zip(first, second):
            self.assertAlmostEqual(a,b,places=places)

    def test_asymmetric_axes(self):
        self.assertEqual(v.transform(v.BASIS, (2,3,5,1)), (2,5,-3,1))
        self.assertEqual(v.transform(v.BASIS_INVERSE, (2,5,-3,1)), (2,3,5,1))

    def test_world_translation_is_conjugated_for_engine_mesh_coordinates(self):
        model = list(v.IDENTITY); model[12:15] = (2,3,5)
        transformed = v.transform(v.to_engine_local(model), (7,11,-13,1))
        self.assertEqual(transformed, (9,16,-16,1))

    def test_camera_and_geometry_share_identical_view_space(self):
        # Camera rotated around Blender Z and translated; asymmetric point catches axis/sign mistakes.
        camera = (0,1,0,0, -1,0,0,0, 0,0,1,0, 3,-2,7,1)
        point = (2,5,-3,1)
        self.close(v.transform(v.to_engine_view(camera), v.transform(v.BASIS, point)),
                   v.transform(camera, point))

    def test_parent_inverse_preserves_nonuniform_mirror_and_shear(self):
        parent = (-2,0,0,0, 0,3,0,0, 0,0,4,0, 8,9,10,1)
        a = math.sqrt(.5)
        local = (a,a,0,0, -a,a,0,0, 0,0,1,0, 1,2,3,1)
        world = v.multiply(parent, local)
        recovered = v.local_from_world(world, parent)
        self.close(recovered, local)
        self.close(v.multiply(v.to_engine_local(parent), v.to_engine_local(recovered)),
                   v.to_engine_local(world))

    def test_non_affine_and_singular_inputs_are_refused(self):
        for index,value in ((3,1e-15),(15,0),(0,0)):
            bad=list(v.IDENTITY); bad[index]=value
            with self.assertRaises(v.UnsupportedView): v.to_engine_local(bad)
        with self.assertRaises(v.UnsupportedView): v.local_from_world(v.IDENTITY,[0]*16)

    def test_near_and_far_are_reversed_zero_to_one(self):
        host=v.perspective(projection(), .1,100,640,480)
        for distance,expected in ((.1,1),(100,0)):
            clip=v.transform(host,(0,0,-distance,1))
            self.assertAlmostEqual(clip[2]/clip[3],expected,places=7)

    def test_reprojection_matches_blender_for_interior_samples(self):
        blender=projection(); host=v.perspective(blender,.1,100,640,480)
        for point in ((.1,.1,-.5,1),(-1,2,-5,1),(10,-8,-70,1)):
            hc=v.transform(host,point); bc=v.transform(blender,point)
            actual=v.depth_to_blender(host,blender,hc[0]/hc[3],hc[1]/hc[3],hc[2]/hc[3],clip_zero_to_one=False)
            self.assertAlmostEqual(actual,.5*bc[2]/bc[3]+.5,places=10)

    def test_orthographic_preserves_xy_and_reverses_linear_depth(self):
        blender=orthographic()
        host=v.orthographic(blender,.1,100,640,480)
        self.assertEqual(host,v.projection(blender,.1,100,640,480))
        for index in range(16):
            if index not in (10,14):
                self.assertEqual(host[index],v.float_matrix(blender)[index])
        for distance,expected in ((.1,1),(50.05,.5),(100,0)):
            clip=v.transform(host,(2,1,-distance,1))
            self.close(clip[:2],(0,0),places=7)
            self.assertEqual(clip[3],1)
            self.assertAlmostEqual(clip[2],expected,places=7)
        self.assertEqual(v.projection(projection(),.1,100,640,480),
                         v.perspective(projection(),.1,100,640,480))

    def test_orthographic_depth_reprojects_both_explicit_backend_conventions(self):
        blender=orthographic(); host=v.orthographic(blender,.1,100,640,480)
        for point in ((-1,-1,-.5,1),(2,1,-50,1),(5,3,-90,1)):
            hc=v.transform(host,point); bc=v.transform(blender,point)
            actual=v.depth_to_blender(host,blender,hc[0],hc[1],hc[2],clip_zero_to_one=False)
            self.assertAlmostEqual(actual,.5*bc[2]+.5,places=10)
            self.assertAlmostEqual(v.depth_to_blender(host,host,hc[0],hc[1],hc[2],
                                                     clip_zero_to_one=True),hc[2],places=10)

    def test_orthographic_refuses_oblique_hybrid_unbounded_or_wrong_aspect(self):
        for index,value in ((8,.1),(1,.1),(11,-1),(15,0),(0,0),
                            (0,1e-7),(5,1001),(12,1e7)):
            p=list(orthographic());p[index]=value
            with self.assertRaises(v.UnsupportedView):v.orthographic(p,.1,100,640,480)
            with self.assertRaises(v.UnsupportedView):v.projection(p,.1,100,640,480)
        with self.assertRaises(v.UnsupportedView):v.orthographic(orthographic(),.1,100,640,360)

    def test_orthographic_maximum_span_accepts_its_observed_float32_matrix(self):
        blender=list(orthographic())
        blender[0],blender[5],blender[12],blender[13]=1e-6,4e-6/3,0,0
        blender=v.float_matrix(blender)
        host=v.projection(blender,.1,100,640,480)
        self.assertEqual(host[0],blender[0])
        self.assertEqual(host[5],blender[5])

    def test_zero_to_one_backend_is_explicit(self):
        host=v.perspective(projection(),.1,100,640,480)
        self.assertAlmostEqual(v.depth_to_blender(host,host,.2,-.3,.75,clip_zero_to_one=True),.75)
        with self.assertRaises(v.UnsupportedView):
            v.depth_to_blender(host,host,0,0,0,clip_zero_to_one=True)
        with self.assertRaises(v.UnsupportedView):
            v.depth_to_blender(host,host,0,0,.2,clip_zero_to_one="guess")

    def test_shifted_orthographic_and_wrong_aspect_views_refused(self):
        for index,value in ((8,.1),(9,-.2),(11,0),(15,1)):
            p=list(projection());p[index]=value
            with self.assertRaises(v.UnsupportedView):v.perspective(p,.1,100,640,480)
        with self.assertRaises(v.UnsupportedView):v.perspective(projection(),.1,100,640,360)

    def test_limits_and_nonfinite_values_refused(self):
        for width,height,near,far in ((1281,720,.1,100),(640,721,.1,100),
                (True,480,.1,100),(0,480,.1,100),(640,480,0,100),
                (640,480,.1,1e7),(640,480,1,1),(640,480,float("nan"),100)):
            with self.assertRaises(v.UnsupportedView):v.perspective(projection(),near,far,width,height)
        for value in (float("inf"),float("nan"),True):
            p=list(v.IDENTITY);p[0]=value
            with self.assertRaises(v.UnsupportedView):v.matrix(p)


if __name__ == "__main__": unittest.main()
