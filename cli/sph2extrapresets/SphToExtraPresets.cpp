/// \brief CLI tool that parses a .sph project file and converts it into a ready-to-use C++ REGISTER_PRESET code block.

#include "Sph.h"
#include "io/FileSystem.h"
#include "run/Config.h"
#include "run/Node.h"
#include "run/jobs/GeometryJobs.h"
#include "run/jobs/InitialConditionJobs.h"
#include "run/jobs/IoJobs.h"
#include "run/jobs/MaterialJobs.h"
#include "run/jobs/ParticleJobs.h"
#include "run/jobs/SimulationJobs.h"
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace Sph;

static void registerAllJobs() {
    // Ensure all job symbols are linked into this binary
    static SphereJob sSphere("");
    static BlockJob sBlock("");
    static EllipsoidJob sEllipsoid("");
    static CylinderJob sCyl("");
    static ToroidJob sTor("");
    static GaussianSphereJob sGauss("");
    static MaterialJob sMat("");
    static MonolithicBodyIc sMono("");
    static DifferentiatedBodyIc sDiff("");
    static SingleParticleIc sSingle("");
    static ImpactorIc sImp("");
    static EquilibriumDensityIc sEq("");
    static CollisionGeometrySetupJob sColl("");
    static JoinParticlesJob sJoin("");
    static SphJob sSph("");
    static SphStabilizationJob sStab("");
    static NBodyJob sNbody("");
    static SaveFileJob sSave("");
    static LoadFileJob sLoad(Path("test.ssf"));
}

struct SettingOverride {
    String name;
    String value;
    bool isEnum = false;
};

// Convert arbitrary string to valid C++ identifier
static String toCppVarName(const String& name) {
    std::string s = name.toUtf8().cstr();
    std::string out;
    for (char c : s) {
        if (std::isalnum((unsigned char)c)) {
            out += c;
        } else {
            if (out.empty() || out.back() != '_') {
                out += '_';
            }
        }
    }
    // Remove trailing underscore if any
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty() || std::isdigit((unsigned char)out[0])) {
        out = "node_" + out;
    }
    return String::fromUtf8(out.c_str());
}

static String sanitizeCppString(const String& str) {
    String s = str;
    s.replaceAll("\\", "\\\\");
    s.replaceAll("\"", "\\\"");
    return s;
}

struct NodeEntry {
    String instanceName;
    String varName;
    String className;
    SharedPtr<JobNode> jobNode;
    UnorderedMap<String, String> connections; // slotName -> providerName
    Array<SettingOverride> overriddenSettings; // key -> value string
};

// Entry visitor to record overridden settings compared to defaults
class SettingCompareProc : public VirtualSettings::IEntryProc {
private:
    const ConfigNode& input;
    Array<SettingOverride>& overrides;

public:
    SettingCompareProc(const ConfigNode& input, Array<SettingOverride>& overrides)
        : input(input)
        , overrides(overrides) {}

    virtual void onCategory(const String& UNUSED(name)) const override {}

    virtual void onEntry(const String& name, IVirtualEntry& entry) const override {
        if (!input.contains(name)) {
            return;
        }

        const IVirtualEntry::Type type = entry.getType();
        try {
            switch (type) {
            case IVirtualEntry::Type::BOOL: {
                bool val = input.get<bool>(name);
                IVirtualEntry::Value defVal = entry.get();
                bool def = defVal.get<bool>();
                if (val != def) {
                    overrides.push(SettingOverride{ name, val ? String("true") : String("false") });
                    entry.set(val);
                }
                break;
            }
            case IVirtualEntry::Type::INT: {
                int val = input.get<int>(name);
                IVirtualEntry::Value defVal = entry.get();
                int def = defVal.get<int>();
                if (val != def) {
                    overrides.push(SettingOverride{ name, toString(val) });
                    entry.set(val);
                }
                break;
            }
            case IVirtualEntry::Type::FLOAT: {
                Float val = input.get<Float>(name);
                IVirtualEntry::Value defVal = entry.get();
                Float def = defVal.get<Float>();
                if (abs(val - def) > 1.e-6_f * max(abs(val), 1._f)) {
                    String strVal = toString(val);
                    if (strVal.find('.') == String::npos && strVal.find('e') == String::npos && strVal.find('E') == String::npos) {
                        strVal += "._f";
                    } else {
                        strVal += "_f";
                    }
                    overrides.push(SettingOverride{ name, strVal });
                    entry.set(val);
                }
                break;
            }
            case IVirtualEntry::Type::VECTOR: {
                Vector val = input.get<Vector>(name);
                IVirtualEntry::Value defVal = entry.get();
                Vector def = defVal.get<Vector>();
                if (val != def) {
                    auto fmtFloat = [](Float f) {
                        String s = toString(f);
                        if (s.find('.') == String::npos && s.find('e') == String::npos && s.find('E') == String::npos) {
                            s += "._f";
                        } else {
                            s += "_f";
                        }
                        return s;
                    };
                    String s = "Vector(" + fmtFloat(val[X]) + ", " + fmtFloat(val[Y]) + ", " +
                               fmtFloat(val[Z]) + ")";
                    overrides.push(SettingOverride{ name, s });
                    entry.set(val);
                }
                break;
            }
            case IVirtualEntry::Type::STRING: {
                String val = input.get<String>(name);
                IVirtualEntry::Value defVal = entry.get();
                String def = defVal.get<String>();
                if (val != def) {
                    overrides.push(SettingOverride{ name, "String(L\"" + sanitizeCppString(val) + "\")" });
                    entry.set(val);
                }
                break;
            }
            case IVirtualEntry::Type::ENUM:
            case IVirtualEntry::Type::FLAGS: {
                int val = input.get<int>(name);
                IVirtualEntry::Value defVal = entry.get();
                EnumWrapper def = defVal.get<EnumWrapper>();
                if (val != def.value) {
                    overrides.push(SettingOverride{ name, toString(val), /* isEnum = */ true });
                    EnumWrapper ew = def;
                    ew.value = val;
                    entry.set(ew);
                }
                break;
            }
            default:
                break;
            }
        } catch (...) {
            // Ignore format mismatch
        }
    }
};

int main(int argc, char* argv[]) {
    registerAllJobs();

    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::cout << "sph2extrapresets - Convert an OpenSPH .sph project file into C++ REGISTER_PRESET code\n\n";
        std::cout << "Usage:\n";
        std::cout << "  sph2extrapresets <input.sph> [--name \"Preset Name\"] [--category \"cat\"] [--append]\n\n";
        std::cout << "Arguments:\n";
        std::cout << "  input.sph          : Path to the .sph project file to convert\n";
        std::cout << "  --name \"Name\"      : Custom display name (defaults to file name)\n";
        std::cout << "  --category \"cat\"   : Category for grouping (defaults to \"imported\")\n";
        std::cout << "  --append           : Append directly into core/run/jobs/ExtraPresets.cpp\n\n";
        std::cout << "Example:\n";
        std::cout << "  sph2extrapresets my_sim.sph --name \"Supernova Impact\" --category \"impacts\"\n";
        std::cout << "  sph2extrapresets my_sim.sph --append\n";
        return 0;
    }

    Path sphPath(String::fromUtf8(argv[1]));
    if (!FileSystem::pathExists(sphPath)) {
        std::cerr << "Error: File '" << sphPath.string().toUtf8().cstr() << "' not found.\n";
        return 1;
    }

    String presetName = sphPath.native();
    // Strip directory and extension
    {
        String base = sphPath.fileName().removeExtension().string();
        base.replaceAll("_", " ");
        base.replaceAll("-", " ");
        presetName = base;
    }

    String category = "imported";
    bool doAppend = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--append") {
            doAppend = true;
        } else if (arg == "--name" && i + 1 < argc) {
            presetName = String::fromUtf8(argv[++i]);
        } else if (arg == "--category" && i + 1 < argc) {
            category = String::fromUtf8(argv[++i]);
        }
    }

    Config config;
    try {
        config.load(sphPath);
    } catch (const Exception& e) {
        std::cerr << "Error parsing .sph file: " << e.what() << "\n";
        return 1;
    }

    SharedPtr<ConfigNode> inNodes = config.getNode("nodes");
    if (!inNodes) {
        std::cerr << "Error: No 'nodes' section found in .sph file.\n";
        return 1;
    }

    FlatMap<String, NodeEntry> nodeEntries;

    // First pass: create all jobs and record overrides
    inNodes->enumerateChildren([&nodeEntries](String name, ConfigNode& nodeConfig) {
        const String className = nodeConfig.get<String>("class_name");
        RawPtr<IJobDesc> desc = getJobDesc(className);
        if (!desc) {
            std::cerr << "Warning: Skipping node '" << name.toUtf8().cstr() << "' (unknown class '"
                      << className.toUtf8().cstr() << "')\n";
            return;
        }

        NodeEntry entry;
        entry.instanceName = name;
        entry.varName = toCppVarName(name);
        entry.className = className;
        entry.jobNode = makeShared<JobNode>(desc->create(name));

        VirtualSettings settings = entry.jobNode->getSettings();
        settings.enumerate(SettingCompareProc(nodeConfig, entry.overriddenSettings));

        for (Size i = 0; i < entry.jobNode->getSlotCnt(); ++i) {
            const String slotName = entry.jobNode->getSlot(i).name;
            Optional<String> connectedName = nodeConfig.tryGet<String>(slotName);
            if (connectedName) {
                entry.connections.insert(slotName, connectedName.value());
            }
        }

        nodeEntries.insert(name, std::move(entry));
    });

    if (nodeEntries.empty()) {
        std::cerr << "Error: No valid nodes could be extracted from the project file.\n";
        return 1;
    }

    // Find the terminal (root) node (a node that is not used as a provider for any other node)
    FlatSet<String> providers;
    for (const auto& pair : nodeEntries) {
        for (const auto& conn : pair.value().connections) {
            providers.insert(conn.value());
        }
    }

    String terminalVarName;
    for (const auto& pair : nodeEntries) {
        if (!providers.contains(pair.key())) {
            terminalVarName = pair.value().varName;
            break;
        }
    }
    if (terminalVarName.empty()) {
        terminalVarName = nodeEntries.begin()->value().varName;
    }

    // Generate C++ code
    std::ostringstream ss;
    ss << "\n// Generated from " << sphPath.fileName().string().toUtf8().cstr() << " via sph2extrapresets\n";
    ss << "REGISTER_PRESET(\n";
    ss << "    \"" << presetName.toUtf8().cstr() << "\",\n";
    ss << "    \"" << category.toUtf8().cstr() << "\",\n";
    ss << "    \"Preset automatically converted from " << sphPath.fileName().string().toUtf8().cstr() << "\",\n";
    ss << "    [](UniqueNameManager& nameMgr, const Size UNUSED(particleCnt)) -> SharedPtr<JobNode> {\n";

    // 1. Create nodes
    ss << "        // 1. Instantiate nodes\n";
    for (const auto& pair : nodeEntries) {
        const NodeEntry& e = pair.value();
        ss << "        SharedPtr<JobNode> " << e.varName.toUtf8().cstr() << " = makeShared<JobNode>(getJobDesc(\""
           << e.className.toUtf8().cstr() << "\")->create(nameMgr.getName(\"" << e.instanceName.toUtf8().cstr() << "\")));\n";
    }
    ss << "\n";

    // 2. Apply settings overrides
    ss << "        // 2. Configure modified settings\n";
    for (const auto& pair : nodeEntries) {
        const NodeEntry& e = pair.value();
        if (!e.overriddenSettings.empty()) {
            ss << "        {\n";
            ss << "            VirtualSettings s = " << e.varName.toUtf8().cstr() << "->getSettings();\n";
            for (Size i = 0; i < e.overriddenSettings.size(); ++i) {
                const auto& setting = e.overriddenSettings[i];
                if (setting.isEnum) {
                    ss << "            setPresetEnum(s, \"" << setting.name.toUtf8().cstr() << "\", " << setting.value.toUtf8().cstr() << ");\n";
                } else {
                    ss << "            setPresetSetting(s, \"" << setting.name.toUtf8().cstr() << "\", " << setting.value.toUtf8().cstr() << ");\n";
                }
            }
            ss << "        }\n";
        }
    }
    ss << "\n";

    // 3. Connect nodes
    ss << "        // 3. Connect node slots\n";
    for (const auto& pair : nodeEntries) {
        const NodeEntry& e = pair.value();
        for (const auto& conn : e.connections) {
            const String& slotName = conn.key();
            const String& providerName = conn.value();
            if (nodeEntries.contains(providerName)) {
                const String& providerVar = nodeEntries[providerName].varName;
                ss << "        " << providerVar.toUtf8().cstr() << "->connect(" << e.varName.toUtf8().cstr() << ", \""
                   << slotName.toUtf8().cstr() << "\");\n";
            }
        }
    }
    ss << "\n";

    // 4. Return root node
    ss << "        // 4. Return execution root node\n";
    ss << "        return " << terminalVarName.toUtf8().cstr() << ";\n";
    ss << "    },\n";
    ss << "    true // isSphSim\n";
    ss << ");\n";

    std::string generatedCode = ss.str();

    std::cout << "\n=======================================================\n";
    std::cout << "Generated Preset Code:\n";
    std::cout << "=======================================================\n";
    std::cout << generatedCode;
    std::cout << "=======================================================\n";

    if (doAppend) {
        Path extraPresetsPath = Path("core/run/jobs/ExtraPresets.cpp");
        if (!FileSystem::pathExists(extraPresetsPath)) {
            // Check if run from build directory
            extraPresetsPath = Path("../../core/run/jobs/ExtraPresets.cpp");
        }
        if (!FileSystem::pathExists(extraPresetsPath)) {
            extraPresetsPath = Path("c:/opensphCommunityedition/core/run/jobs/ExtraPresets.cpp");
        }

        std::string filePath = extraPresetsPath.string().toUtf8().cstr();
        std::ifstream inFile(filePath);
        if (!inFile.is_open()) {
            std::cerr << "Error: Could not open " << filePath << " for appending.\n";
            return 1;
        }
        std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
        inFile.close();

        // Insert before NAMESPACE_SPH_END
        size_t pos = content.rfind("NAMESPACE_SPH_END");
        if (pos != std::string::npos) {
            content.insert(pos, generatedCode + "\n");
        } else {
            content += "\n" + generatedCode;
        }

        std::ofstream outFile(filePath);
        if (!outFile.is_open()) {
            std::cerr << "Error: Could not write to " << filePath << "\n";
            return 1;
        }
        outFile << content;
        outFile.close();
        std::cout << "\nSuccessfully appended preset '" << presetName.toUtf8().cstr() << "' to " << filePath << "!\n";
    } else {
        std::cout << "\nTip: To automatically append this to ExtraPresets.cpp, run:\n";
        std::cout << "  sph2extrapresets " << sphPath.string().toUtf8().cstr() << " --append\n\n";
    }

    return 0;
}
