#include "vfb_plugin_writer.h"
#include "BLI_fileops.h"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

using namespace VRayBaseTypes;

namespace VRayForBlender {

struct TraceTransformHex {
	TraceTransformHex(AttrTransform tm) {
		m[0][0] = tm.m.v0.x;
		m[0][1] = tm.m.v0.y;
		m[0][2] = tm.m.v0.z;

		m[1][0] = tm.m.v1.x;
		m[1][1] = tm.m.v1.y;
		m[1][2] = tm.m.v1.z;

		m[2][0] = tm.m.v2.x;
		m[2][1] = tm.m.v2.y;
		m[2][2] = tm.m.v2.z;

		v[0] = tm.offs.x;
		v[1] = tm.offs.y;
		v[2] = tm.offs.z;
	}

	float  m[3][3];
	double v[3];
};

bool PluginWriter::WriteItem::isDone() const {
	return m_ready.load(std::memory_order_acquire);
}

const char * PluginWriter::WriteItem::getData() const {
	if (m_isAsync) {
		return m_asyncData;
	} else {
		return m_data.c_str();
	}
}

void PluginWriter::WriteItem::asyncDone(const char * data) {
	BLI_assert(m_isAsync && "Called asyncDone on sync WriteItem");
	// first copy data into task
	if (data) {
		m_asyncData = data;
		m_freeData = true;
	} else {
		m_asyncData = "";
	}
	// than mark as ready
	// release-aquire order so readers will see m_asyncData changed if m_ready is true
	m_ready.store(true, std::memory_order_release);
}

PluginWriter::WriteItem::~WriteItem() {
	if (m_freeData) {
		delete[] m_asyncData;
	}
}

PluginWriter::WriteItem::WriteItem(WriteItem && other): WriteItem() {
	std::swap(m_data, other.m_data);
	std::swap(m_asyncData, other.m_asyncData);
	m_ready = other.m_ready.load();
	other.m_ready = false;
	std::swap(m_freeData, other.m_freeData);
	std::swap(m_isAsync, other.m_isAsync);
}


PluginWriter::PluginWriter(ThreadManager::Ptr tm, PyObject *pyFile, ExporterSettings::ExportFormat format)
	: m_threadManager(tm)
    , m_depth(1)
    , m_animationFrame(-FLT_MAX)
    , m_file(pyFile)
    , m_format(format)
{
}

bool PluginWriter::good() const
{
	return m_file != nullptr;
}

#define FormatAndAdd(pp, ...)                                     \
	char buf[2048];                                               \
	sprintf(buf, __VA_ARGS__);                                    \
	pp.writeStr(buf);                                             \
	return pp;                                                    \


PluginWriter &PluginWriter::writeStr(const char *str)
{
	if (good()) {
		addTask(str);
	}
	return *this;
}

void PluginWriter::processItems(const char * val)
{
	// this function will not be called concurrently
	// so it is safe to traverse m_items and expect not to change during execution

	const int maxRep = m_items.size();
	for (int c = 0; c < maxRep && m_items.front().isDone(); ++c) {
		PyObject_CallMethod(m_file, _C("write"), _C("s"), m_items.front().getData());
		m_items.pop_front();
	}

	if (val && *val) {
		if (m_items.empty()) {
			// no items left in que, just write current value
			PyObject_CallMethod(m_file, _C("write"), _C("s"), val);
		} else {
			m_items.push_back(WriteItem(val));
		}
	}
}


void PluginWriter::blockFlushAll()
{
	for (int c = 0; c < m_items.size(); ++c) {
		auto & item = m_items[c];
		// lazy wait for item
		while (!item.isDone()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		PyObject_CallMethod(m_file, _C("write"), _C("s"), item.getData());
	}

	m_items.clear();
}

const char * PluginWriter::indentation()
{
	switch (m_depth) {
	case 0: return "";
	case 1: return VRSCENE_INDENT;
	case 2: return VRSCENE_INDENT VRSCENE_INDENT;
	case 3: return VRSCENE_INDENT VRSCENE_INDENT VRSCENE_INDENT;
	case 4: return VRSCENE_INDENT VRSCENE_INDENT VRSCENE_INDENT VRSCENE_INDENT;
	default: return "";
	}
}

PluginWriter &operator<<(PluginWriter &pp, int val)
{
	FormatAndAdd(pp, "%d", val);
}

PluginWriter &operator<<(PluginWriter &pp, float val)
{
	FormatAndAdd(pp, "%.4f", val);
}

PluginWriter &operator<<(PluginWriter &pp, const char *val)
{
	return *val ? pp.writeStr(val) : pp;
}

PluginWriter &operator<<(PluginWriter &pp, const std::string &val)
{
	return !val.empty() ? pp.writeStr(val.c_str()) : pp;
}

PluginWriter &operator<<(PluginWriter &pp, const AttrColor &val)
{
	FormatAndAdd(pp, "Color(%g,%g,%g)", val.r, val.g, val.b);
}

PluginWriter &operator<<(PluginWriter &pp, const AttrAColor &val)
{
	FormatAndAdd(pp, "AColor(%g,%g,%g,%g)", val.color.r, val.color.g, val.color.b, val.alpha);
}

PluginWriter &operator<<(PluginWriter &pp, const AttrVector &val)
{
	FormatAndAdd(pp, "Vector(%g,%g,%g)", val.x, val.y, val.z);
}

PluginWriter &operator<<(PluginWriter &pp, const AttrVector2 &val)
{
	FormatAndAdd(pp, "Vector(%g,%g,0)", val.x, val.y);
}

PluginWriter &operator<<(PluginWriter &pp, const AttrMatrix &val)
{
	return pp << "Matrix(" << val.v0 << "," << val.v1 << "," << val.v2 << ")";
}

PluginWriter &operator<<(PluginWriter &pp, const AttrTransform &val)
{
	if (pp.format() == ExporterSettings::ExportFormatASCII) {
		pp << "Transform(" << val.m << "," << val.offs << ")";
	}
	else {
		char tmBuf[CGR_TRANSFORM_HEX_SIZE];
		TraceTransformHex tm(val);

		const u_int8_t *tm8 = (const u_int8_t*)&tm;
		getStringHex(tm8, sizeof(TraceTransformHex), tmBuf);

		pp << "TransformHex(\"" << tmBuf << "\")";
	}
	return pp;
}

PluginWriter &operator<<(PluginWriter &pp, const AttrPlugin &val)
{
	pp << StripString(val.plugin);
	if (!val.output.empty()) {
		pp << "::" << StripString(val.output);
	}
	return pp;
}

PluginWriter &operator<<(PluginWriter &pp, const AttrMapChannels &val)
{
	pp << "List(\n";

	if (!val.data.empty()) {
		pp.indent();
		int index = 0;
		bool addComma = false;
		for (auto iter = val.data.cbegin(); iter != val.data.cend(); ++iter) {
			pp << pp.indentation() << (addComma ? "," : "") << "List(" << index++ << ",\n";
			pp << pp.indent();
			pp << iter->second.vertices << ",\n" << pp.indentation() << iter->second.faces;
			pp.unindent();
			pp << "\n" << pp.indentation() << ")\n";
			addComma = true;
		}
		pp.unindent();
	}
	return pp << pp.indentation() << ")";
}

PluginWriter &operator<<(PluginWriter &pp, const AttrInstancer &val)
{
	pp << "List(" << val.frameNumber;

	if (!val.data.empty()) {
		pp << ",\n" << pp.indent() << "List(" << (*val.data)[0].index << ", " << (*val.data)[0].tm << ", " << (*val.data)[0].vel << "," << (*val.data)[0].node << ")";
		for (int c = 1; c < val.data.getCount(); c++) {
			const auto &item = (*val.data)[c];
			pp << ",\n" << pp.indentation() << "List(" << item.index << ", " << item.tm << ", " << item.vel << "," << item.node << ")";
		}
		pp.unindent();
	}

	return pp << "\n" << pp.indentation() << ")";
}

PluginWriter &operator<<(PluginWriter &pp, const VRayBaseTypes::AttrListValue &val)
{
	return printList(pp, val, "", val.getCount() > 10 ? 2 : 0);
}

PluginWriter &operator<<(PluginWriter &pp, const VRayBaseTypes::AttrValue &val)
{
	switch (val.type) {
	case ValueTypeInt: return pp << val.as<AttrSimpleType<int>>();
	case ValueTypeFloat: return pp << val.as<AttrSimpleType<float>>();
	case ValueTypeString: return pp <<  val.as<AttrSimpleType<std::string>>();
	case ValueTypeColor: return pp << val.as<AttrColor>();
	case ValueTypeVector: return pp << val.as<AttrVector>();
	case ValueTypeAColor: return pp << val.as<AttrAColor>();
	case ValueTypePlugin: return pp << val.as<AttrPlugin>();
	case ValueTypeTransform: return pp << val.as<AttrTransform>();
	case ValueTypeMatrix: return pp << val.as<AttrMatrix>();
	case ValueTypeListInt: return pp << val.as<AttrListInt>();
	case ValueTypeListFloat: return pp << val.as<AttrListFloat>();
	case ValueTypeListVector: return pp << val.as<AttrListVector>();
	case ValueTypeListColor: return pp << val.as<AttrListColor>();
	case ValueTypeListPlugin: return pp << val.as<AttrListPlugin>();
	case ValueTypeListString: return pp << val.as<AttrListString>();
	case ValueTypeMapChannels: return pp << val.as<AttrMapChannels>();
	case ValueTypeInstancer: return pp << val.as<AttrInstancer>();
	case ValueTypeListValue: return pp << val.as<AttrListValue>();
	default:
		BLI_assert(!"Unsupported attribute type");
		break;
	}
	return pp;
}

template <>
PluginWriter &printList(PluginWriter &pp, const VRayBaseTypes::AttrList<std::string> &val, const char *listName, int itemsPerLine)
{
	pp << "List" << listName;

	if (val.empty()) {
		return pp << "()";
	}
	itemsPerLine = std::max(0, itemsPerLine);
	pp << "(";
	if (itemsPerLine) {
		pp << "\n" << pp.indent();
	}
	pp << "\"" << (*val)[0] << "\"";
	for (int c = 1; c < val.getCount(); c++) {
		pp << ",";
		if (itemsPerLine && c % itemsPerLine == 0) {
			pp << "\n" << pp.indentation();
		} else {
			pp << " ";
		}
		pp <<"\"" << (*val)[c] << "\"";
	}
	if (itemsPerLine) {
		pp.unindent();
		pp << "\n" << pp.indentation() << ")";
	}
	return pp;
}


template <> inline
PluginWriter &operator<<(PluginWriter &pp, const KVPair<std::string> &val)
{
	return pp << pp.indent() << val.first << "=\"" << val.second << "\";\n" << pp.unindent();
}

template <> inline
PluginWriter &operator<<(PluginWriter &pp, const VRayBaseTypes::AttrSimpleType<std::string> &val)
{
	return pp << "\"" << val.value << "\"";
}

template <> inline
PluginWriter &operator<<(PluginWriter &pp, const VRayBaseTypes::AttrList<float> &val)
{
	return printList(pp, val, "Float");
}

template <> inline
PluginWriter &operator<<(PluginWriter &pp, const VRayBaseTypes::AttrList<int> &val)
{
	return printList(pp, val, "Int");
}

template <> inline
PluginWriter &operator<<(PluginWriter &pp, const VRayBaseTypes::AttrList<VRayBaseTypes::AttrVector> &val)
{
	return printList(pp, val, "Vector", 1);
}

} // VRayForBlender