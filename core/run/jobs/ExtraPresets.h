#pragma once

#include "run/Node.h"

NAMESPACE_SPH_BEGIN

class UniqueNameManager;

/// \brief Callback to construct a preset node graph.
using PresetFactoryFunc = Function<SharedPtr<JobNode>(UniqueNameManager& nameMgr, const Size particleCnt)>;

/// \brief Metadata and factory for a registered custom preset.
struct CustomPresetDesc {
    String name;         ///< Display name shown in GUI (e.g. "Giant Impact")
    String category;     ///< Category / Subfolder name (e.g. "planetary", "collisions")
    String tooltip;      ///< Description / tooltip
    PresetFactoryFunc factory; ///< Node builder function
    bool isSphSim = true;      ///< If true, sets optimal default particle rendering radius
};

/// \brief Returns a view of all registered custom presets.
ArrayView<const CustomPresetDesc> enumerateCustomPresets();

/// \brief Helper struct to self-register presets at binary startup.
struct PresetRegistrar {
    PresetRegistrar(String name,
        String category,
        String tooltip,
        PresetFactoryFunc factory,
        bool isSphSim = true);
};

/// \brief Helper to set setting values polymorphically across enums, flags, and primitives.
inline void setPresetSetting(VirtualSettings& settings, const String& name, const bool val) {
    settings.set(name, val);
}
inline void setPresetSetting(VirtualSettings& settings, const String& name, const int val) {
    settings.set(name, val);
}
inline void setPresetSetting(VirtualSettings& settings, const String& name, const Float val) {
    settings.set(name, val);
}
inline void setPresetSetting(VirtualSettings& settings, const String& name, const Vector& val) {
    settings.set(name, val);
}
inline void setPresetSetting(VirtualSettings& settings, const String& name, const String& val) {
    settings.set(name, val);
}
inline void setPresetSetting(VirtualSettings& settings, const String& name, const char* val) {
    settings.set(name, String::fromAscii(val));
}
inline void setPresetEnum(VirtualSettings& settings, const String& name, const int val) {
    RawPtr<IVirtualEntry> entry = settings.getEntry(name);
    if (entry) {
        EnumWrapper ew = entry->get().get<EnumWrapper>();
        ew.value = val;
        entry->set(ew);
    }
}

#define SPH_PRESET_CONCAT_IMPL(x, y) x##y
#define SPH_PRESET_CONCAT(x, y) SPH_PRESET_CONCAT_IMPL(x, y)

/// \brief Macro to define and self-register a preset cleanly in ExtraPresets.cpp
#define REGISTER_PRESET(Name, Category, Tooltip, FactoryLambda, ...)                                         \
    static ::Sph::PresetRegistrar SPH_PRESET_CONCAT(sRegisterPreset_, __COUNTER__)(                          \
        Name, Category, Tooltip, FactoryLambda, ##__VA_ARGS__)

NAMESPACE_SPH_END
