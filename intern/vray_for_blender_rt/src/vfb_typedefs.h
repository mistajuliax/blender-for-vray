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

#ifndef VRAY_FOR_BLENDER_STD_TYPEDEFS_H
#define VRAY_FOR_BLENDER_STD_TYPEDEFS_H

#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

#include "vfb_rna.h"
#include <RNA_blender_cpp.h>

// do this here so it is availabe everywhere we might use hash map/set
namespace std {
	template <> struct hash<BL::Object> {
		size_t operator()(BL::Object ob) const {
			return std::hash<std::string>()(ob.name());
		}
	};

	template <> struct hash<BL::Material> {
		size_t operator()(BL::Material mat) const {
			return std::hash<std::string>()(mat.name());
		}
	};
};

typedef std::vector<std::string> StrVector;

template <typename KeyT, typename HashT = std::hash<KeyT>>
using HashSet = std::unordered_set<KeyT, HashT>;

template <typename KeyT, typename ValT, typename HashT = std::hash<KeyT>>
using HashMap = std::unordered_map<KeyT, ValT, HashT>;


typedef HashSet<std::string> StringHashSet;


#endif // VRAY_FOR_BLENDER_STD_TYPEDEFS_H
