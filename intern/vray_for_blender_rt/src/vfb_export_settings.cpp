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

#include "vfb_export_settings.h"
#include "cgr_config.h"
#include "utils/vfb_utils_blender.h"

#include <boost/asio/ip/host_name.hpp>


using namespace VRayForBlender;


ExporterSettings::ExporterSettings()
    : export_meshes(true)
    , override_material(PointerRNA_NULL)
    , current_bake_object(PointerRNA_NULL)
    , camera_stereo_left(PointerRNA_NULL)
    , camera_stereo_right(PointerRNA_NULL)
    , background_scene(PointerRNA_NULL)
{}


void ExporterSettings::update(BL::Context context, BL::RenderEngine engine, BL::BlendData data, BL::Scene _scene, BL::SpaceView3D view3d)
{
	is_viewport = !!view3d;
	is_preview = engine && engine.is_preview();

	BL::Scene scene(_scene);
	if (is_preview) {
		scene = context.scene();
	}

	settings_dr.init(scene);
	settings_dr.use = settings_dr.use && !is_preview; // && !isViewport; viewport DR?

	background_scene = BL::Scene(RNA_pointer_get(&scene.ptr, "background_set"));

	m_vrayScene    = RNA_pointer_get(&scene.ptr, "vray");
	m_vrayExporter = RNA_pointer_get(&m_vrayScene, "Exporter");

	calculate_instancer_velocity = RNA_boolean_get(&m_vrayExporter, "calculate_instancer_velocity");
	export_hair         = RNA_boolean_get(&m_vrayExporter, "use_hair");
	export_fluids       = RNA_boolean_get(&m_vrayExporter, "use_smoke");
	use_displace_subdiv = RNA_boolean_get(&m_vrayExporter, "use_displace");
	use_select_preview  = RNA_boolean_get(&m_vrayExporter, "select_node_preview");
	use_subsurf_to_osd  = RNA_boolean_get(&m_vrayExporter, "subsurf_to_osd");
	default_mapping     = (DefaultMapping)RNA_enum_ext_get(&m_vrayExporter, "default_mapping");
	export_meshes       = is_preview ? true : RNA_boolean_get(&m_vrayExporter, "auto_meshes");
	export_file_format  = (ExportFormat)RNA_enum_ext_get(&m_vrayExporter, "data_format");
	if (is_preview) {
		// force zip for preview so it can be faster if we are writing to file
		export_file_format = ExportFormat::ExportFormatZIP;
	}

	PointerRNA BakeView = RNA_pointer_get(&m_vrayScene, "BakeView");
	use_bake_view = RNA_boolean_get(&BakeView, "use") && !is_viewport && !is_preview; // no bake in viewport
	if (use_bake_view) {
		PointerRNA bakeObj = RNA_pointer_get(&m_vrayExporter, "currentBakeObject");
		current_bake_object = BL::Object(bakeObj);
	}

	if (!current_bake_object) {
		use_bake_view = false;
	}

	settings_files.use_separate  = RNA_boolean_get(&m_vrayExporter, "useSeparateFiles");
	settings_files.output_type   = (SettingsFiles::OutputDirType)RNA_enum_ext_get(&m_vrayExporter, "output");
	settings_files.output_dir    = RNA_std_string_get(&m_vrayExporter, "output_dir");
	settings_files.output_unique = RNA_boolean_get(&m_vrayExporter, "output_unique");
	settings_files.project_path  = data.filepath();

	use_active_layers = static_cast<ActiveLayers>(RNA_enum_get(&m_vrayExporter, "activeLayers"));
	if (is_preview) {
		// preview scene's layers are actually the different objects that the scene is showinge
		use_active_layers = ActiveLayersScene;
	}
	// Read layers if custom are specified
	if(use_active_layers == ActiveLayersCustom) {
		RNA_boolean_get_array(&m_vrayExporter, "customRenderLayers", active_layers.data);
	}

	if (is_preview || is_viewport || use_bake_view) {
		settings_animation.mode = SettingsAnimation::AnimationMode::AnimationModeNone;
	} else {
		settings_animation.mode = (SettingsAnimation::AnimationMode)RNA_enum_get(&m_vrayExporter, "animation_mode");
	}

	settings_animation.use = settings_animation.mode != SettingsAnimation::AnimationModeNone;

	settings_animation.frame_start   = scene.frame_start();
	settings_animation.frame_current = scene.frame_current();
	settings_animation.frame_step    = scene.frame_step();

	use_stereo_camera = false;
	use_motion_blur = false;
	use_physical_camera = false;

	// Find if we need hide from view
	if (settings_animation.mode == SettingsAnimation::AnimationModeCameraLoop) {
		for (auto ob : Blender::collection(scene.objects)) {
			if (ob.type() == BL::Object::type_CAMERA) {
				auto dataPtr = ob.data().ptr;
				PointerRNA vrayCamera = RNA_pointer_get(&dataPtr, "vray");

				if (RNA_boolean_get(&vrayCamera, "use_camera_loop")) {
					if (RNA_boolean_get(&vrayCamera, "hide_from_view")) {
						use_hide_from_view = true;
						break;
					}
				}
			}
		}
	} else {
		BL::Object camera(scene.camera());
		// NOTE: Could happen if scene has no camera and we initing exporter for
		// proxy export, for example.
		if (camera && camera.type() == BL::Object::type_CAMERA) {
			BL::Camera camera_data(camera.data());

			PointerRNA vrayCamera = RNA_pointer_get(&camera_data.ptr, "vray");
			PointerRNA physCamera = RNA_pointer_get(&vrayCamera, "CameraPhysical");
			use_physical_camera = RNA_boolean_get(&physCamera, "use");

			PointerRNA stereoSettings = RNA_pointer_get(&m_vrayScene, "VRayStereoscopicSettings");
			PointerRNA cameraStereo = RNA_pointer_get(&vrayCamera, "CameraStereoscopic");
			use_stereo_camera = (stereoSettings.data && RNA_boolean_get(&stereoSettings, "use")) && (cameraStereo.data && RNA_boolean_get(&cameraStereo, "use"));
			if (use_stereo_camera) {
				const auto leftCamName = RNA_std_string_get(&cameraStereo, "LeftCam");
				const auto rightCamName = RNA_std_string_get(&cameraStereo, "RightCam");

				BL::BlendData::objects_iterator obIt;
				int found = 0;
				for (data.objects.begin(obIt); found < 2 && obIt != data.objects.end(); ++obIt) {
					const auto camName = obIt->name();
					if (camName == leftCamName) {
						camera_stereo_left = *obIt;
						found++;
					} else if (camName == rightCamName) {
						camera_stereo_right = *obIt;
						found++;
					}
				}
				if (found != 2) {
					use_stereo_camera = false;
					PRINT_ERROR("Failed to find cameras for stereo camera!");
				}
			}

			use_hide_from_view = RNA_boolean_get(&vrayCamera, "hide_from_view");
			PointerRNA mbSettings = RNA_pointer_get(&vrayCamera, "SettingsMotionBlur");
			mb_samples = RNA_int_get(&mbSettings, "geom_samples");

			if (use_physical_camera) {
				use_motion_blur = RNA_boolean_get(&physCamera, "use_moblur");
				enum PhysicalCameraType {
					Still = 0, Cinematic = 1, Video = 2,
				};
				const auto cameraType = static_cast<PhysicalCameraType>(RNA_enum_ext_get(&physCamera, "type"));
				const float frameDuration = 1.0 / (scene.render().fps() / scene.render().fps_base());

				if (cameraType == Still) {
					mb_duration = 1.0 / (RNA_float_get(&physCamera, "shutter_speed") * frameDuration);
					mb_offset = mb_duration * 0.5;
				} else if (cameraType == Cinematic) {
					mb_duration = RNA_float_get(&physCamera, "shutter_angle") / 360.0;
					mb_offset = RNA_float_get(&physCamera, "shutter_offset") / 360.0 + mb_duration * 0.5;
				} else if (cameraType == Video) {
					mb_duration = 1.0 + RNA_float_get(&physCamera, "latency") / frameDuration;
					mb_offset = -mb_duration * 0.5;
				} else {
					use_motion_blur = false;
				}
			} else {
				if (RNA_boolean_get(&mbSettings, "on")) {
					use_motion_blur = true;
					mb_duration = RNA_float_get(&mbSettings, "duration");
					mb_offset = RNA_float_get(&mbSettings, "interval_center");
				}
			}
		}
	}

	// disable motion blur for bake render
	use_motion_blur = use_motion_blur && !use_bake_view && !is_preview;

	std::string overrideName;
	PointerRNA settingsOptions = RNA_pointer_get(&m_vrayScene, "SettingsOptions");
	if(RNA_boolean_get(&settingsOptions, "mtl_override_on")) {
		overrideName = RNA_std_string_get(&settingsOptions, "mtl_override");
	}

	if(overrideName != override_material_name) {
		override_material_name = overrideName;
		if (override_material_name.empty()) {
			override_material = BL::Material(PointerRNA_NULL);
		} else {
			for (auto & mat : Blender::collection(data.materials)) {
				if (mat.name() == overrideName) {
					override_material = mat;
					break;
				}
			}
		}
	}

	exporter_type = (ExporterType)RNA_enum_get(&m_vrayExporter, "backend");
	if (exporter_type != ExporterType::ExpoterTypeFile) {
		// there is no sense to skip meshes for other than file export
		export_meshes = true;
	}

	work_mode = (WorkMode)RNA_enum_get(&m_vrayExporter, "work_mode");

	zmq_server_port    = RNA_int_get(&m_vrayExporter, "zmq_port");
	zmq_server_address = RNA_std_string_get(&m_vrayExporter, "zmq_address");
	if (zmq_server_address.empty()) {
		zmq_server_address = "127.0.0.1";
	}

	if (is_viewport) {
		render_mode = static_cast<RenderMode>(RNA_enum_ext_get(&m_vrayExporter, "viewport_rendering_mode"));
	} else {
		render_mode = static_cast<RenderMode>(RNA_enum_ext_get(&m_vrayExporter, "rendering_mode"));
		if (is_preview || settings_animation.use) {
			render_mode = RenderMode::RenderModeProduction;
		}
	}

	m_viewportResolution = RNA_int_get(&m_vrayExporter, "viewport_resolution") / 100.0f;
	viewport_image_quality = RNA_int_get(&m_vrayExporter, "viewport_jpeg_quality");
	if (is_viewport) {
		viewport_image_type = static_cast<ImageType>(RNA_enum_ext_get(&m_vrayExporter, "viewport_image_type"));
	} else {
		viewport_image_type = ImageType::RGBA_REAL;
	}
	show_vfb = !is_viewport && work_mode != WorkMode::WorkModeExportOnly && !is_preview && RNA_boolean_get(&m_vrayExporter, "display");
	close_on_stop = RNA_boolean_get(&m_vrayExporter, "autoclose");

	verbose_level = static_cast<VRayVerboseLevel>(RNA_enum_ext_get(&m_vrayExporter, "verboseLevel"));
}


bool ExporterSettings::check_data_updates()
{
	bool do_update_check = false;
	if(settings_animation.use && settings_animation.frame_current > settings_animation.frame_start) {
		do_update_check = true;
	}
	return do_update_check;
}


bool ExporterSettings::is_first_frame()
{
	bool is_first_frame = false;
	if(!settings_animation.use) {
		is_first_frame = true;
	}
	else if(settings_animation.frame_current == settings_animation.frame_start) {
		is_first_frame = true;
	}
	return is_first_frame;
}


int ExporterSettings::getViewportShowAlpha()
{
	return RNA_boolean_get(&m_vrayExporter, "viewport_alpha");
}


SettingsDR::SettingsDR():
    use(false)
{
	hostname = boost::asio::ip::host_name();
}

void SettingsDR::init(BL::Scene scene)
{
	PointerRNA vrayScene = RNA_pointer_get(&scene.ptr, "vray");
	PointerRNA vrayDR = RNA_pointer_get(&vrayScene, "VRayDR");

	use = RNA_boolean_get(&vrayDR, "on");

	share_name = RNA_std_string_get(&vrayDR, "share_name");

	network_type = (NetworkType)RNA_enum_get(&vrayDR, "networkType");
	sharing_type = (SharingType)RNA_enum_get(&vrayDR, "assetSharing");
}
