# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

import bpy
import sys
import os
import tempfile
import traceback
import inspect


def print_fail_msg_and_exit(msg):
    def __LINE__():
        try:
            raise Exception
        except:
            return sys.exc_info()[2].tb_frame.f_back.f_back.f_back.f_lineno

    def __FILE__():
        return inspect.currentframe().f_code.co_filename

    print("'%s': %d >> %s" % (__FILE__(), __LINE__(), msg), file=sys.stderr)
    sys.stderr.flush()
    os._exit(1)


def abort_if_false(expr, msg=None):
    if not expr:
        if not msg:
            msg = "test failed"
            print_fail_msg_and_exit(msg)


class TestClass(bpy.types.PropertyGroup):
    prop = bpy.props.PointerProperty(type=bpy.types.Object)
    name = bpy.props.StringProperty(name="name")


def select_objects(ob_arr):
    for o in bpy.data.objects:
        if o.name in ob_arr:
            o.select = True
        else:
            o.select = False


def check_crash(fnc):
    try:
        fnc()
    except:
        return
    print_fail_msg_and_exit("test failed")


def register_common_classes():
    bpy.utils.register_class(TestClass)
    bpy.types.Object.prop_array = bpy.props.CollectionProperty(
        name="prop_array",
        type=TestClass,
        description="test")

def test1():
    bpy.types.Object.prop_alone = bpy.props.PointerProperty(type=bpy.types.Object)

    arr_len = 100; ob_cp_count = 100

    # Make one named copy of the cube
    select_objects(["Cube"])
    bpy.ops.object.duplicate()
    select_objects(["Cube.001"])
    bpy.context.active_object.name = "Unique_Cube"
    select_objects(["Cube"])

    bpy.context.active_object.prop_alone = bpy.data.objects['Camera']
    # check value
    abort_if_false(bpy.context.active_object.prop_alone == bpy.data.objects['Camera'])

    # check arrays
    for i in range(0, arr_len):
        a = bpy.context.active_object.prop_array.add()
        a.prop = bpy.data.objects['Lamp']

    for i in range(0, arr_len):
        abort_if_false(bpy.context.active_object.prop_array[i].prop == bpy.data.objects['Lamp'])

    # check duplicating
    for i in range(0, ob_cp_count):
        bpy.ops.object.duplicate()

    # nodes
    bpy.data.scenes["Scene"].use_nodes = True
    bpy.data.scenes["Scene"].node_tree.nodes['Render Layers']["prop"] = bpy.data.objects['Camera']

    # rename scene and save
    bpy.data.scenes["Scene"].name = "Scene_lib"
    tempdir = tempfile.gettempdir()
    libpath = filepath = os.path.join(tempdir, "lib.blend")
    bpy.ops.wm.save_as_mainfile(filepath=filepath)

    # open startup file
    bpy.ops.wm.read_homefile()

    # link just one Cube, expecting that 'Camera' object will be linked too
    with bpy.data.libraries.load(filepath, link=True) as (data_from, data_to):
        data_to.objects.append("Unique_Cube")
    o = bpy.data.objects["Unique_Cube"]
    bpy.context.scene.objects.link(o)
    abort_if_false(o.prop_alone)
    # both must be from the same library
    abort_if_false(o.prop_alone.library == o.library)

    # link library to the startup file
    with bpy.data.libraries.load(filepath, link=True) as (data_from, data_to):
        data_to.scenes = ["Scene_lib"]

    # check the value of the first element of array
    abort_if_false(
        bpy.data.scenes["Scene_lib"].objects['Unique_Cube'].prop_array[0].prop ==
        bpy.data.scenes["Scene_lib"].objects['Lamp'])

    # full copy of the scene with datablock props
    bpy.ops.wm.open_mainfile(filepath=filepath)

    # IDP_CopyID, IDP_RelinkProperty etc
    bpy.ops.scene.new(type='FULL_COPY')

    select_objects(["Lamp"])
    bpy.ops.object.delete(use_global=False)

    filepath = os.path.join(tempdir, "test.blend")
    bpy.ops.wm.save_as_mainfile(filepath=filepath)
    bpy.ops.wm.open_mainfile(filepath=filepath)

    # check nodes props
    abort_if_false(
        bpy.data.scenes["Scene_lib"].node_tree.nodes['Render Layers']["prop"] ==
        bpy.data.scenes["Scene_lib.001"].node_tree.nodes['Render Layers']["prop"])

def test2():
    # just panel for testing poll with lots of objects
    class TEST_PT_DatablockProp(bpy.types.Panel):
        bl_label = "Datablock IDProp"
        bl_space_type = "PROPERTIES"
        bl_region_type = "WINDOW"
        bl_context = "scene"

        def draw(self, context):
            self.layout.template_ID(context.scene, "prop")
            # for correct behavior it is better to allow collections of pointers
            # but now it is not necessary
            self.layout.prop_search(context.scene, "prop1", bpy.context.active_object,
                                    "prop_array", text="Currently failing")
            self.layout.prop_search(context.scene, "prop2", bpy.data, "node_groups")

    bpy.utils.register_class(TEST_PT_DatablockProp)

    def poll(self, value):
        return value.name in bpy.data.scenes["Scene_lib"].objects

    def poll1(self, value):
        return True

    bpy.types.Scene.prop = bpy.props.PointerProperty(type=bpy.types.Object, poll=poll)
    bpy.types.Scene.prop1 = bpy.props.PointerProperty(type=bpy.types.Object)
    bpy.types.Scene.prop2 = bpy.props.PointerProperty(type=bpy.types.NodeTree, poll=poll1)

    # check poll effect on UI (poll returns false => red alert)
    bpy.context.scene.prop = bpy.data.objects["Lamp.001"]
    bpy.context.scene.prop1 = bpy.data.objects["Lamp.001"]

    # check incorrect type assignment
    def sub_test():
        bpy.context.scene.prop2 = bpy.data.objects["Lamp.001"]

    check_crash(sub_test)

    bpy.context.scene.prop2 = bpy.data.node_groups.new("Shader", "ShaderNodeTree")

    print("Please, test GUI performance manually on the Scene tab, '%s' panel" % TEST_PT_DatablockProp.bl_label,
          file=sys.stderr)


def test3():
    bpy.types.Object.prop_gr = bpy.props.PointerProperty(
        name="prop_gr",
        type=TestClass,
        description="test")

    bpy.data.objects["Unique_Cube"].prop_gr = None


def test4():
    bpy.types.Object.prop_str = bpy.props.StringProperty(name="str")

    select_objects(["Unique_Cube"])
    bpy.data.objects["Unique_Cube"].prop_str = "test"


def main():
    register_common_classes()
    test1()
    test2()
    check_crash(test3)
    test4()


if __name__ == "__main__":
    try:
        main()
    except:
        import traceback

        traceback.print_exc()
        sys.stderr.flush()
        os._exit(1)
