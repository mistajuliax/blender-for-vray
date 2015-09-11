/*
 * Copyright (c) 2015, Chaos Software Ltd
 *
 * V-Ray For Blender
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cgr_config.h"

#include "vfb_util_defines.h"

#include "vfb_scene_exporter.h"
#include "vfb_utils_nodes.h"
#include "vfb_utils_blender.h"
#include "vfb_utils_math.h"

#include "BLI_rect.h"
#include "DNA_view3d_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_object_types.h"
#include "BKE_camera.h"

extern "C" {
#include "BKE_idprop.h"
#include "BKE_node.h" // For ntreeUpdateTree()
}

#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/format.hpp>

#include <ctime>
#include <chrono>
#include <thread>

/* OpenGL header includes, used everywhere we use OpenGL, to deal with
 * platform differences in one central place. */

#ifdef WITH_GLEW_MX
#  include "glew-mx.h"
#else
#  include <GL/glew.h>
#  define mxCreateContext() glewInit()
#  define mxMakeCurrentContext(x) (x)
#endif


using namespace VRayForBlender;


static StrSet RenderSettingsPlugins;
static StrSet RenderGIPlugins;


SceneExporter::SceneExporter(BL::Context context, BL::RenderEngine engine, BL::BlendData data, BL::Scene scene, BL::SpaceView3D view3d, BL::RegionView3D region3d, BL::Region region):
    m_context(context),
    m_engine(engine),
    m_data(data),
    m_scene(scene),
    m_view3d(view3d),
    m_region3d(region3d),
    m_region(region),
    m_exporter(nullptr)
{
	if (!RenderSettingsPlugins.size()) {
		RenderSettingsPlugins.insert("SettingsOptions");
		RenderSettingsPlugins.insert("SettingsColorMapping");
		RenderSettingsPlugins.insert("SettingsDMCSampler");
		RenderSettingsPlugins.insert("SettingsImageSampler");
		RenderSettingsPlugins.insert("SettingsGI");
		RenderSettingsPlugins.insert("SettingsIrradianceMap");
		RenderSettingsPlugins.insert("SettingsLightCache");
		RenderSettingsPlugins.insert("SettingsDMCGI");
		RenderSettingsPlugins.insert("SettingsRaycaster");
		RenderSettingsPlugins.insert("SettingsRegionsGenerator");
#if 0
		RenderSettingsPlugins.insert("SettingsOutput");
		RenderSettingsPlugins.insert("SettingsRTEngine");
#endif
	}

	if (!RenderGIPlugins.size()) {
		RenderGIPlugins.insert("SettingsGI");
		RenderGIPlugins.insert("SettingsLightCache");
		RenderGIPlugins.insert("SettingsIrradianceMap");
		RenderGIPlugins.insert("SettingsDMCGI");
	}
	m_settings.init(m_data, m_scene);
}


SceneExporter::~SceneExporter()
{
	free();
}


void SceneExporter::init()
{
	create_exporter();
	if (!m_exporter) {
		PRINT_INFO_EX("Failed to create exporter!");
	}
	assert(m_exporter && "Failed to create exporter!");

	if (m_exporter) {
		m_exporter->set_callback_on_image_ready(ExpoterCallback(boost::bind(&SceneExporter::tag_redraw, this)));
		m_exporter->set_callback_on_rt_image_updated(ExpoterCallback(boost::bind(&SceneExporter::tag_redraw, this)));

		// directly bind to the engine
		m_exporter->set_callback_on_message_updated(boost::bind(&BL::RenderEngine::update_stats, &m_engine, _1, _2));

		m_exporter->init();
	}

	m_data_exporter.init(m_exporter, m_settings);
	m_data_exporter.init_data(m_data, m_scene, m_engine, m_context);
	m_data_exporter.init_defaults();
}


void SceneExporter::create_exporter()
{
	m_exporter = ExporterCreate(m_settings.exporter_type);
	if (!m_exporter) {
		m_exporter = ExporterCreate(ExpoterType::ExporterTypeInvalid);
		if (!m_exporter) {
			return;
		}
	}
}


void SceneExporter::free()
{
	PluginDesc::cache.clear();
	ExporterDelete(m_exporter);
}


void SceneExporter::resize(int w, int h)
{
	PRINT_INFO_EX("SceneExporter->resize(%i, %i)",
	              w, h);

	m_exporter->set_render_size(w, h);
}


void SceneExporter::draw()
{
	sync_view(true);

	RenderImage image = m_exporter->get_image();
	if (!image) {
		tag_redraw();
		return;
	}

	const bool transparent = false;

	glPushMatrix();

	glTranslatef(m_viewParams.render_size.offs_x, m_viewParams.render_size.offs_y, 0.0f);

	if (transparent) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}

	glColor3f(1.0f, 1.0f, 1.0f);

#if 1
	GLuint texid;
	glGenTextures(1, &texid);
	glBindTexture(GL_TEXTURE_2D, texid);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, image.w, image.h, 0, GL_RGBA, GL_FLOAT, image.pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glEnable(GL_TEXTURE_2D);

	glPushMatrix();
	glTranslatef(0.0f, 0.0f, 0.0f);

	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2f(0.0f, 0.0f);
	glTexCoord2f(1.0f, 1.0f);
	glVertex2f((float)m_viewParams.render_size.w, 0.0f);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2f((float)m_viewParams.render_size.w, (float)m_viewParams.render_size.h);
	glTexCoord2f(0.0f, 0.0f);
	glVertex2f(0.0f, (float)m_viewParams.render_size.h);
	glEnd();

	glPopMatrix();

	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &texid);
#else
	/* fallback for old graphics cards that don't support GLSL, half float,
	 * and non-power-of-two textures */
	glRasterPos2f(0.0f, 0.0f);
	glDrawPixels(m_viewParams.render_size.w, m_viewParams.render_size.h, GL_RGBA, GL_FLOAT, image.pixels);
	glRasterPos2f(0.0f, 0.0f);
#endif

	if (transparent) {
		glDisable(GL_BLEND);
	}

	glPopMatrix();

	image.free();
}


void SceneExporter::render_start()
{
	if (m_settings.work_mode == ExporterSettings::WorkMode::WorkModeRender ||
	    m_settings.work_mode == ExporterSettings::WorkMode::WorkModeRenderAndExport) {
		m_exporter->start();
	}
}


bool SceneExporter::export_animation()
{
	using namespace std;
	using namespace std::chrono;

	const float frame = m_scene.frame_current();

	m_settings.settings_animation.frame_current = frame;
	m_exporter->set_current_frame(frame);

	PRINT_INFO_EX("Exporting animation frame %d", m_scene.frame_current());
	m_exporter->stop();
	sync(false);
	m_exporter->start();

	auto lastTime = high_resolution_clock::now();
	while (m_exporter->get_last_rendered_frame() < frame) {
		this_thread::sleep_for(milliseconds(1));

		auto now = high_resolution_clock::now();
		if (duration_cast<seconds>(now - lastTime).count() > 1) {
			lastTime = now;
			PRINT_INFO_EX("Waiting for renderer to render animation frame %f, current %f", frame, m_exporter->get_last_rendered_frame());
		}
		if (this->is_interrupted()) {
			PRINT_INFO_EX("Interrupted - stopping animation rendering!");
			return false;
		}
		if (m_exporter->is_aborted()) {
			PRINT_INFO_EX("Renderer stopped - stopping animation rendering!");
			return false;
		}
	}

	return true;
}


void SceneExporter::sync(const int &check_updated)
{
	PRINT_INFO_EX("SceneExporter->sync(%i)",
	              check_updated);

	clock_t begin = clock();

	sync_prepass();

	PointerRNA vrayScene = RNA_pointer_get(&m_scene.ptr, "vray");

	for (const auto &pluginID : RenderSettingsPlugins) {
		PointerRNA propGroup = RNA_pointer_get(&vrayScene, pluginID.c_str());

		PluginDesc pluginDesc(pluginID, pluginID);

		m_data_exporter.setAttrsFromPropGroupAuto(pluginDesc, &propGroup, pluginID);

		m_exporter->export_plugin(pluginDesc);
	}

	sync_view(check_updated);
	sync_materials(check_updated);
	sync_objects(check_updated);
	sync_effects(check_updated);

	m_data_exporter.sync();

	clock_t end = clock();

	double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;

	PRINT_INFO_EX("Synced in %.3f sec.",
	              elapsed_secs);

	// Sync data (will remove deleted objects)
	m_exporter->sync();

	// Export stuff after sync
	if (m_settings.work_mode == ExporterSettings::WorkMode::WorkModeExportOnly ||
	    m_settings.work_mode == ExporterSettings::WorkMode::WorkModeRenderAndExport) {
		const std::string filepath = "scene_app_sdk.vrscene";
		m_exporter->export_vrscene(filepath);
	}
}


static void TagNtreeIfIdPropTextureUpdated(BL::NodeTree ntree, BL::Node node, const std::string &texAttr)
{
	BL::Texture tex(Blender::GetDataFromProperty<BL::Texture>(&node.ptr, texAttr));
	if (tex && (tex.is_updated() || tex.is_updated_data())) {
		PRINT_INFO_EX("Texture %s is updated...",
		              tex.name().c_str());
		DataExporter::tag_ntree(ntree);
	}
}


void SceneExporter::sync_prepass()
{
	m_data_exporter.m_id_cache.clear();
	m_data_exporter.m_id_track.reset_usage();

	BL::BlendData::node_groups_iterator nIt;
	for (m_data.node_groups.begin(nIt); nIt != m_data.node_groups.end(); ++nIt) {
		BL::NodeTree ntree(*nIt);
		bNodeTree *_ntree = (bNodeTree*)ntree.ptr.data;

		if (IDP_is_ID_used((ID*)_ntree)) {
			if (boost::starts_with(ntree.bl_idname(), "VRayNodeTree")) {
				// NOTE: On scene save node links are not properly updated for some
				// reason; simply manually update everything...
				ntreeUpdateTree((Main*)m_data.ptr.data, _ntree);

				// Check nodes
				BL::NodeTree::nodes_iterator nodeIt;
				for (ntree.nodes.begin(nodeIt); nodeIt != ntree.nodes.end(); ++nodeIt) {
					BL::Node node(*nodeIt);
					if (node.bl_idname() == "VRayNodeMetaImageTexture" ||
					    node.bl_idname() == "VRayNodeBitmapBuffer"     ||
					    node.bl_idname() == "VRayNodeTexGradRamp"      ||
					    node.bl_idname() == "VRayNodeTexRemap") {
						TagNtreeIfIdPropTextureUpdated(ntree, node, "texture");
					}
					else if (node.bl_idname() == "VRayNodeTexSoftBox") {
						TagNtreeIfIdPropTextureUpdated(ntree, node, "ramp_grad_vert");
						TagNtreeIfIdPropTextureUpdated(ntree, node, "ramp_grad_horiz");
						TagNtreeIfIdPropTextureUpdated(ntree, node, "ramp_grad_rad");
						TagNtreeIfIdPropTextureUpdated(ntree, node, "ramp_frame");
					}
				}
			}
		}
	}
}


static float GetLensShift(BL::Object ob)
{
	float shift = 0.0f;

	BL::Constraint constraint(PointerRNA_NULL);
	if (ob.constraints.length()) {
		BL::Object::constraints_iterator cIt;
		for (ob.constraints.begin(cIt); cIt != ob.constraints.end(); ++cIt) {
			BL::Constraint cn(*cIt);

			if ((cn.type() == BL::Constraint::type_TRACK_TO)     ||
			    (cn.type() == BL::Constraint::type_DAMPED_TRACK) ||
			    (cn.type() == BL::Constraint::type_LOCKED_TRACK)) {
				constraint = cn;
				break;
			}
		}
	}

	if (constraint) {
		BL::ConstraintTarget ct(constraint);
		BL::Object target(ct.target());
		if (target) {
			const float z_shift = ob.matrix_world().data[14] - target.matrix_world().data[14];
			const float l = Blender::GetDistanceObOb(ob, target);
			shift = -1.0f * z_shift / l;
		}
	}
	else {
		const float rx  = ob.rotation_euler().data[0];
		const float lsx = rx - M_PI_2;
		if (fabs(lsx) > 0.0001f) {
			shift = tanf(lsx);
		}
		if (fabs(shift) > M_PI) {
			shift = 0.0f;
		}
	}

	return shift;
}


void SceneExporter::sync_view(const int &check_updated)
{
	if (!m_scene.camera() && !m_view3d) {
		PRINT_ERROR("Unable to setup view!")
	}
	else {
		ViewParams viewParams;

		viewParams.render_view.tm = BL::Object(m_scene.camera()).matrix_world();
		if (!m_view3d) {
			PRINT_ERROR("Final frame render is not supported.")
		}
		else {
			if(m_region3d.view_perspective() == BL::RegionView3D::view_perspective_CAMERA) {
				BL::Object camera = m_view3d.lock_camera_and_layers()
				                    ? m_scene.camera()
				                    : m_view3d.camera();

				if (!camera) {
					PRINT_ERROR("Camera is not found!")
				}
				else {
					rctf view_border;

					// NOTE: Taken from source/blender/editors/space_view3d/view3d_draw.c:
					// static void view3d_camera_border(...) {...}
					//
					bool no_zoom = false;
					bool no_shift = false;

					Scene *scene = (Scene *)m_scene.ptr.data;
					const ARegion *ar = (const ARegion*)m_region.ptr.data;
					const View3D *v3d = (const View3D *)m_view3d.ptr.data;
					const RegionView3D *rv3d = (const RegionView3D *)m_region3d.ptr.data;

					CameraParams params;
					rctf rect_view, rect_camera;

					/* get viewport viewplane */
					BKE_camera_params_init(&params);
					BKE_camera_params_from_view3d(&params, v3d, rv3d);
					if (no_zoom)
						params.zoom = 1.0f;
					BKE_camera_params_compute_viewplane(&params, ar->winx, ar->winy, 1.0f, 1.0f);
					rect_view = params.viewplane;

					/* get camera viewplane */
					BKE_camera_params_init(&params);
					/* fallback for non camera objects */
					params.clipsta = v3d->near;
					params.clipend = v3d->far;
					BKE_camera_params_from_object(&params, v3d->camera);
					if (no_shift) {
						params.shiftx = 0.0f;
						params.shifty = 0.0f;
					}
					BKE_camera_params_compute_viewplane(&params, scene->r.xsch, scene->r.ysch, scene->r.xasp, scene->r.yasp);
					rect_camera = params.viewplane;

					/* get camera border within viewport */
					view_border.xmin = ((rect_camera.xmin - rect_view.xmin) / BLI_rctf_size_x(&rect_view)) * ar->winx;
					view_border.xmax = ((rect_camera.xmax - rect_view.xmin) / BLI_rctf_size_x(&rect_view)) * ar->winx;
					view_border.ymin = ((rect_camera.ymin - rect_view.ymin) / BLI_rctf_size_y(&rect_view)) * ar->winy;
					view_border.ymax = ((rect_camera.ymax - rect_view.ymin) / BLI_rctf_size_y(&rect_view)) * ar->winy;

					viewParams.render_size.offs_x = view_border.xmin;
					viewParams.render_size.offs_y = view_border.ymin;
					// NOTE: +2 to match camera border
					viewParams.render_size.w = view_border.xmax - view_border.xmin + 2;
					viewParams.render_size.h = view_border.ymax - view_border.ymin + 2;

					const float aspect = float(viewParams.render_size.w) / float(viewParams.render_size.h);

					BL::Object camera(m_scene.camera());
					BL::Camera camera_data(camera.data());

					PointerRNA vrayCamera = RNA_pointer_get(&camera_data.ptr, "vray");

					PointerRNA renderView = RNA_pointer_get(&vrayCamera, "RenderView");

					viewParams.render_view.fov = RNA_boolean_get(&vrayCamera, "override_fov")
					                             ? RNA_float_get(&vrayCamera, "fov")
					                             : camera_data.angle();

					viewParams.render_view.ortho = (camera_data.type() == BL::Camera::type_ORTHO);
					viewParams.render_view.ortho_width = camera_data.ortho_scale();

					if (aspect < 1.0f) {
						viewParams.render_view.fov = 2.0f * atanf(tanf(viewParams.render_view.fov / 2.0f) * aspect);
						viewParams.render_view.ortho_width *= aspect;
					}

					viewParams.render_view.use_clip_start = RNA_boolean_get(&renderView, "clip_near");
					viewParams.render_view.use_clip_end   = RNA_boolean_get(&renderView, "clip_far");

					viewParams.render_view.clip_start = camera_data.clip_start();
					viewParams.render_view.clip_end   = camera_data.clip_end();

					viewParams.render_view.tm  = camera.matrix_world();

					PointerRNA physicalCamera = RNA_pointer_get(&vrayCamera, "CameraPhysical");
					if (RNA_boolean_get(&physicalCamera, "use")) {
						float horizontal_offset = -camera_data.shift_x();
						float vertical_offset   = -camera_data.shift_y();
						if (aspect < 1.0f) {
							const float offset_fix = 1.0 / aspect;
							horizontal_offset *= offset_fix;
							vertical_offset   *= offset_fix;
						}

						const float lens_shift = RNA_boolean_get(&physicalCamera, "auto_lens_shift")
						                         ? GetLensShift(camera)
						                         : RNA_float_get(&physicalCamera, "lens_shift");

						float focus_distance = Blender::GetCameraDofDistance(camera);
						if (focus_distance < 0.001f) {
							focus_distance = 5.0f;
						}

						PluginDesc physCamDesc("cameraPhysical", "CameraPhysical");
						physCamDesc.add("fov", viewParams.render_view.fov);
						physCamDesc.add("horizontal_offset", horizontal_offset);
						physCamDesc.add("vertical_offset",   vertical_offset);
						physCamDesc.add("lens_shift",        lens_shift);
						physCamDesc.add("focus_distance",    focus_distance);

						m_data_exporter.setAttrsFromPropGroupAuto(physCamDesc, &physicalCamera, "CameraPhysical");
						m_exporter->export_plugin(physCamDesc);
					}
				}
			}
			else {
				BL::Object camera_obj = (m_view3d.lock_camera_and_layers()) ? m_scene.camera() : m_view3d.camera();
				BL::Camera camera(camera_obj.data());

				const auto & sensor_size = (camera.sensor_fit() == BL::Camera::sensor_fit_VERTICAL) ? camera.sensor_height() : camera.sensor_width();

				viewParams.render_size.offs_x = 0;
				viewParams.render_size.offs_y = 0;
				viewParams.render_size.w = m_region.width();
				viewParams.render_size.h = m_region.height();

				float lens = m_view3d.lens() / 2.f;

				viewParams.render_view.ortho = (m_region3d.view_perspective() == BL::RegionView3D::view_perspective_ORTHO);
				viewParams.render_view.ortho_width = m_region3d.view_distance() * sensor_size / lens;

				const ARegion *ar = (const ARegion*)m_region.ptr.data;
				float aspect = 0.f;

				if (viewParams.render_view.ortho) {
					aspect = viewParams.render_view.ortho_width / 2.0f;
				} else {
					lens /= 2.f;
					aspect = float(ar->winx) / float(ar->winy);
				}


				viewParams.render_view.fov = 2.0f * atanf((0.5f * sensor_size) / lens / aspect);

				viewParams.render_view.use_clip_start = true;
				viewParams.render_view.use_clip_end   = true;

				viewParams.render_view.clip_start = m_view3d.clip_start();
				viewParams.render_view.clip_end = m_view3d.clip_end();

				viewParams.render_view.tm = Math::InvertTm(m_region3d.view_matrix());
			}
		}

		if (m_viewParams.size_changed(viewParams)) {
			// PRINT_WARN("View resize: %i x %i", viewParams.render_size.w, viewParams.render_size.h);
			resize(m_viewParams.render_size.w, m_viewParams.render_size.h);
		}
		if (m_viewParams.pos_changed(viewParams)) {
			// PRINT_WARN("Pos change: %i x %i", viewParams.render_size.offs_x, viewParams.render_size.offs_y);
			tag_redraw();
		}
		if (m_viewParams.params_changed(viewParams)) {
			// PRINT_WARN("View update: fov = %.3f", viewParams.render_view.fov);

			PluginDesc viewDesc("renderView", "RenderView");
			viewDesc.add("transform", AttrTransformFromBlTransform(viewParams.render_view.tm));

			if (!m_view3d) {
				viewDesc.add("fov", m_viewParams.render_view.fov);
				viewDesc.add("clipping",     (m_viewParams.render_view.use_clip_start || m_viewParams.render_view.use_clip_end));
				viewDesc.add("clipping_near", m_viewParams.render_view.clip_start);
				viewDesc.add("clipping_far",  m_viewParams.render_view.clip_end);

				viewDesc.add("orthographic", m_viewParams.render_view.ortho);
				viewDesc.add("orthographicWidth", m_viewParams.render_view.ortho_width);
			} else {
				viewDesc.add("fov", viewParams.render_view.fov);
				viewDesc.add("clipping",     (viewParams.render_view.use_clip_start || viewParams.render_view.use_clip_end));
				viewDesc.add("clipping_near", viewParams.render_view.clip_start);
				viewDesc.add("clipping_far",  viewParams.render_view.clip_end);

				viewDesc.add("orthographic", viewParams.render_view.ortho);
				viewDesc.add("orthographicWidth", viewParams.render_view.ortho_width);
			}


			m_exporter->export_plugin(viewDesc);

			if (m_ortho_camera != static_cast<bool>(viewParams.render_view.ortho) && m_exporter->is_running()) {
				m_exporter->stop();
				m_exporter->start();
			}

			m_ortho_camera = viewParams.render_view.ortho;
		}
	}
}


void SceneExporter::sync_materials(const int &check_updated)
{
	PRINT_INFO_EX("SceneExporter->sync_materials(%i)",
	              check_updated);

	BL::BlendData::materials_iterator maIt;
	for (m_data.materials.begin(maIt); maIt != m_data.materials.end(); ++maIt) {
		BL::Material ma(*maIt);
		BL::NodeTree ntree = Nodes::GetNodeTree(ma);
		if (!ntree) {
			// PRINT_ERROR("");
		}
		else {
			const bool is_updated = check_updated
			                        ? (ma.is_updated() || ntree.is_updated())
			                        : true;

			if (is_updated) {
				m_data_exporter.exportMaterial(ma);
			}

			DataExporter::tag_ntree(ntree, false);
		}
	}
}


unsigned int SceneExporter::get_layer(BlLayers array)
{
	unsigned int layer = 0;

	for(unsigned int i = 0; i < 20; i++)
		if (array[i])
			layer |= (1 << i);

	return layer;
}


void SceneExporter::sync_object(BL::Object ob, const int &check_updated, const ObjectOverridesAttrs & override)
{
	bool add = false;
	if (override) {
		add = !m_data_exporter.m_id_cache.contains(override.id);
	} else {
		add = !m_data_exporter.m_id_cache.contains(ob);
	}

	if (add) {
		if (override.override) {
			m_data_exporter.m_id_cache.insert(override.id);
		} else {
			m_data_exporter.m_id_cache.insert(ob);
		}

		PointerRNA vrayObject = RNA_pointer_get(&ob.ptr, "vray");

		bool is_on_visible_layer = get_layer(ob.layers()) & get_layer(m_scene.layers());
		bool is_hidden = ob.hide() || ob.hide_render() || !is_on_visible_layer;

		// const int data_updated = RNA_int_get(&vrayObject, "data_updated");

		if ((!is_hidden)) {
			PRINT_INFO_EX("Syncing: %s...",
			              ob.name().c_str());

			if (ob.data() && ob.type() == BL::Object::type_MESH) {
				m_data_exporter.exportObject(ob, check_updated, override);
			}
			else if (ob.data() && ob.type() == BL::Object::type_LAMP) {
				m_data_exporter.exportLight(ob, check_updated, override);
			}
		}

		// Reset update flag
		RNA_int_set(&vrayObject, "data_updated", CGR_NONE);
	}
}

static int ob_has_dupli(BL::Object ob) {
	return ((ob.dupli_type() != BL::Object::dupli_type_NONE) && (ob.dupli_type() != BL::Object::dupli_type_FRAMES));
}

static int ob_is_duplicator_renderable(BL::Object ob) {
	bool is_renderable = true;

	// Dulpi
	if (ob_has_dupli(ob)) {
		PointerRNA vrayObject = RNA_pointer_get(&ob.ptr, "vray");
		is_renderable = RNA_boolean_get(&vrayObject, "dupliShowEmitter");
	}

	// Particles
	// Particle system "Show / Hide Emitter" has priority over dupli
	if (ob.particle_systems.length()) {
		is_renderable = true;

		BL::Object::modifiers_iterator mdIt;
		for (ob.modifiers.begin(mdIt); mdIt != ob.modifiers.end(); ++mdIt) {
			BL::Modifier md(*mdIt);
			if (md.type() == BL::Modifier::type_PARTICLE_SYSTEM) {
				BL::ParticleSystemModifier pmod(md);
				BL::ParticleSystem psys(pmod.particle_system());
				if (psys) {
					BL::ParticleSettings pset(psys.settings());
					if (pset) {
						if (!pset.use_render_emitter()) {
							is_renderable = false;
							break;
						}
					}
				}
			}
		}
	}

	return is_renderable;
}


void SceneExporter::sync_dupli(BL::Object ob, const int &check_updated)
{
	PointerRNA vrayObject = RNA_pointer_get(&ob.ptr, "vray");
	const int dupli_override_id   = RNA_int_get(&vrayObject, "dupliGroupIDOverride");
	const int dupli_use_instancer = RNA_boolean_get(&vrayObject, "use_instancer");

	AttrInstancer instances;
	instances.frameNumber = 0;
	if (dupli_use_instancer) {
		int num_instances = 0;

		BL::Object::dupli_list_iterator dupIt;
		for (ob.dupli_list.begin(dupIt); dupIt != ob.dupli_list.end(); ++dupIt) {
			BL::DupliObject dupliOb(*dupIt);
			BL::Object      dupOb(dupliOb.object());

			const bool is_hidden = dupliOb.hide() || dupOb.hide_render();
			const bool is_light = Blender::IsLight(dupOb);

			if (!is_hidden && !is_light) {
				num_instances++;
			}
		}

		instances.data.resize(num_instances);
	}

	if (is_interrupted()) {
		return;
	}

	BL::Object::dupli_list_iterator dupIt;
	int dupli_instance = 0;
	for (ob.dupli_list.begin(dupIt); dupIt != ob.dupli_list.end(); ++dupIt) {
		if (is_interrupted()) {
			return;
		}

		BL::DupliObject dupliOb(*dupIt);
		BL::Object      dupOb(dupliOb.object());

		const bool is_hidden = dupliOb.hide() || dupOb.hide_render();

		const bool is_light = Blender::IsLight(dupOb);
		const bool supported_type = Blender::IsGeometry(dupOb) || is_light;

		MHash persistendID;
		MurmurHash3_x86_32((const void*)dupIt->persistent_id().data, 8 * sizeof(int), 42, &persistendID);

		if (!is_hidden && supported_type) {
			if (is_light) {
				ObjectOverridesAttrs overrideAttrs;

				overrideAttrs.override = true;
				overrideAttrs.visible = true;
				overrideAttrs.tm = AttrTransformFromBlTransform(dupliOb.matrix());
				overrideAttrs.id = persistendID;

				char namePrefix[255] = {0, };
				namePrefix[0] = 'D';
				snprintf(namePrefix + 1, 250, "%u", persistendID);
				strcat(namePrefix, "@");
				strcat(namePrefix, ob.name().c_str());

				overrideAttrs.namePrefix = namePrefix;

				sync_object(dupOb, check_updated, overrideAttrs);
			}
			else if (dupli_use_instancer) {
				ObjectOverridesAttrs overrideAttrs;
				overrideAttrs.override = true;
				// If dupli are shown via Instancer we need to hide
				// original object
				overrideAttrs.visible = ob_is_duplicator_renderable(dupOb);
				overrideAttrs.tm = AttrTransformFromBlTransform(dupOb.matrix_world());
				overrideAttrs.id = reinterpret_cast<int>(dupOb.ptr.data);

				float inverted[4][4];
				copy_m4_m4(inverted, ((Object*)dupOb.ptr.data)->obmat);
				invert_m4(inverted);

				float tm[4][4];
				mul_m4_m4m4(tm, ((DupliObject*)dupliOb.ptr.data)->mat, inverted);

				AttrInstancer::Item &instancer_item = (*instances.data)[dupli_instance];
				instancer_item.index = persistendID;

				instancer_item.node = m_data_exporter.getNodeName(dupOb);
				instancer_item.tm = AttrTransformFromBlTransform(tm);

				dupli_instance++;

				sync_object(dupOb, check_updated, overrideAttrs);
			}
		}
	}

	if (dupli_use_instancer) {
		static boost::format InstancerFmt("Dupli@%s");

		PluginDesc instancerDesc(boost::str(InstancerFmt % m_data_exporter.getNodeName(ob)), "Instancer");
		instancerDesc.add("instances", instances);

		m_exporter->export_plugin(instancerDesc);
	}
}


void SceneExporter::sync_objects(const int &check_updated)
{
	PRINT_INFO_EX("SceneExporter->sync_objects(%i)",
	              check_updated);

	// TODO:
	// [ ] Track new objects (creation / layer settings change)
	// [ ] Track deleted objects

	// Sync objects
	//
	BL::Scene::objects_iterator obIt;
	for (m_scene.objects.begin(obIt); obIt != m_scene.objects.end(); ++obIt) {
		if (is_interrupted()) {
			break;
		}

		BL::Object ob(*obIt);

		if (ob.is_duplicator()) {
			sync_dupli(ob);
			if (is_interrupted()) {
				break;
			}

			ObjectOverridesAttrs overAttrs;

			overAttrs.override = true;
			overAttrs.id = reinterpret_cast<int>(ob.ptr.data);
			overAttrs.tm = AttrTransformFromBlTransform(ob.matrix_world());
			overAttrs.visible = ob_is_duplicator_renderable(ob);

			sync_object(ob, check_updated, overAttrs);
		} else {
			sync_object(ob, check_updated);
		}
	}
}


void SceneExporter::sync_effects(const int &check_updated)
{
	NodeContext ctx;
	m_data_exporter.exportVRayEnvironment(ctx);
}


void SceneExporter::tag_update()
{
	/* tell blender that we want to get another update callback */
	m_engine.tag_update();
}


void SceneExporter::tag_redraw()
{
#if 0
	if (background) {
		/* update stats and progress, only for background here because
		 * in 3d view we do it in draw for thread safety reasons */
		update_status_progress();

		/* offline render, redraw if timeout passed */
		if (time_dt() - last_redraw_time > 1.0) {
			b_engine.tag_redraw();
			last_redraw_time = time_dt();
		}
	}
	else {
#endif
		/* tell blender that we want to redraw */
		m_engine.tag_redraw();
#if 0
	}
#endif
}


int SceneExporter::is_interrupted()
{
	bool export_interrupt = false;
	if (m_engine && m_engine.test_break()) {
		export_interrupt = true;
	}
	return export_interrupt;
}
